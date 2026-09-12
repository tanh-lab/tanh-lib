#include "HttpBackend.h"

#import <Foundation/Foundation.h>

#include <cstdio>

/// NSURLSession is asynchronous; the caller's thread blocks on a semaphore that
/// the delegate signals from the session queue.
///
/// The body is streamed, never buffered: an 85 MB asset must not sit in memory
/// on a phone, so each chunk is appended to the destination as it arrives.
/// Small in-memory fetches (manifests) opt in explicitly via `body`.
@interface TanhDownloadDelegate : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, assign) std::FILE* file;
@property(nonatomic, assign) std::uint64_t received;
@property(nonatomic, assign) std::uint64_t resumeOffset;
@property(nonatomic, assign) std::uint64_t expected;
@property(nonatomic, assign) int httpCode;
@property(nonatomic, assign) BOOL cancelledByCaller;
@property(nonatomic, assign) BOOL writeFailed;
@property(nonatomic, assign) BOOL bodyTooLarge;
@property(nonatomic, strong) NSMutableData* body;
@property(nonatomic, assign) std::size_t maxBodyBytes;
@property(nonatomic, assign) const std::atomic<bool>* cancelled;
@property(nonatomic, assign) const thl::net::HttpProgressFn* onProgress;
@property(nonatomic, strong) NSError* failure;
@property(nonatomic, strong) NSString* destinationPath;
@property(nonatomic, assign) dispatch_semaphore_t done;
@end

@implementation TanhDownloadDelegate

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    auto* http = (NSHTTPURLResponse*)response;
    self.httpCode = (int)http.statusCode;

    if (self.httpCode < 200 || self.httpCode >= 300) {
        completionHandler(NSURLSessionResponseCancel);
        return;
    }

    // 200 where we asked for a range means the server ignored Range and is
    // sending the whole file. Anything already on disk is not a prefix of this
    // body, so start the file over rather than appending to it.
    if (self.resumeOffset > 0 && self.httpCode == 200) {
        if (self.file != nullptr) { std::fclose(self.file); }
        self.file = std::fopen(self.destinationPath.UTF8String, "wb");
        self.resumeOffset = 0;
        if (self.file == nullptr) {
            self.writeFailed = YES;
            completionHandler(NSURLSessionResponseCancel);
            return;
        }
    }

    const long long length = http.expectedContentLength;
    self.expected = length > 0 ? (std::uint64_t)length : 0;
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveData:(NSData*)data {
    if (self.cancelled != nullptr && self.cancelled->load(std::memory_order_relaxed)) {
        self.cancelledByCaller = YES;
        [dataTask cancel];
        return;
    }

    if (self.body != nil) {
        if (self.maxBodyBytes > 0 && self.body.length + data.length > self.maxBodyBytes) {
            self.bodyTooLarge = YES;
            [dataTask cancel];
            return;
        }
        [self.body appendData:data];
    } else if (self.file != nullptr) {
        __block BOOL failed = NO;
        [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
            if (std::fwrite(bytes, 1, range.length, self.file) != range.length) {
                failed = YES;
                *stop = YES;
            }
        }];
        if (failed) {
            self.writeFailed = YES;
            [dataTask cancel];
            return;
        }
    }

    self.received += data.length;

    if (self.onProgress != nullptr && *self.onProgress) {
        thl::net::HttpProgress progress;
        progress.m_downloaded = self.resumeOffset + self.received;
        progress.m_total = self.expected > 0 ? self.resumeOffset + self.expected : 0;
        if (!(*self.onProgress)(progress)) {
            self.cancelledByCaller = YES;
            [dataTask cancel];
        }
    }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
    self.failure = error;
    dispatch_semaphore_signal(self.done);
}

@end

namespace thl::net::detail {

HttpResult perform(const Request& request) {
    HttpResult result;

    @autoreleasepool {
        NSString* const url_string = [NSString stringWithUTF8String:request.m_url.c_str()];
        if (url_string == nil) {
            result.m_status = HttpStatus::NetworkError;
            result.m_message = "URL is not valid UTF-8.";
            return result;
        }
        NSURL* const url = [NSURL URLWithString:url_string];
        if (url == nil) {
            result.m_status = HttpStatus::NetworkError;
            result.m_message = "Malformed URL.";
            return result;
        }

        const bool to_file = !request.m_destination.empty();
        const std::uint64_t resume_from = to_file ? request.m_resume_from : 0;

        TanhDownloadDelegate* const delegate = [[TanhDownloadDelegate alloc] init];
        delegate.cancelled = request.m_cancelled;
        delegate.onProgress = request.m_on_progress;
        delegate.resumeOffset = resume_from;
        delegate.done = dispatch_semaphore_create(0);

        if (to_file) {
            delegate.destinationPath =
                [NSString stringWithUTF8String:request.m_destination.string().c_str()];
            delegate.file =
                std::fopen(delegate.destinationPath.UTF8String, resume_from > 0 ? "ab" : "wb");
            if (delegate.file == nullptr) {
                result.m_status = HttpStatus::FileError;
                result.m_message = "Could not open the destination for writing.";
                return result;
            }
        } else {
            delegate.body = [NSMutableData data];
            delegate.maxBodyBytes = request.m_max_body_bytes;
        }

        NSURLSessionConfiguration* const configuration =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.timeoutIntervalForRequest =
            request.m_timeout_seconds > 0 ? request.m_timeout_seconds : 60;
        // Bounds a stall, not the transfer: a large asset on a slow link must
        // not be killed for being big.
        configuration.timeoutIntervalForResource = 60 * 60;

        NSOperationQueue* const queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        NSURLSession* const session = [NSURLSession sessionWithConfiguration:configuration
                                                                    delegate:delegate
                                                               delegateQueue:queue];

        NSMutableURLRequest* const http_request = [NSMutableURLRequest requestWithURL:url];
        http_request.HTTPMethod = @"GET";
        if (resume_from > 0) {
            [http_request setValue:[NSString stringWithFormat:@"bytes=%llu-", resume_from]
                forHTTPHeaderField:@"Range"];
        }

        NSURLSessionDataTask* const task = [session dataTaskWithRequest:http_request];
        [task resume];

        // Wake regularly so a cancel() from another thread is noticed even when
        // the server has gone quiet and no delegate callback is coming.
        while (dispatch_semaphore_wait(delegate.done,
                                       dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
            if (request.m_cancelled != nullptr &&
                request.m_cancelled->load(std::memory_order_relaxed)) {
                delegate.cancelledByCaller = YES;
                [task cancel];
            }
        }

        [session finishTasksAndInvalidate];

        if (delegate.file != nullptr) {
            std::fclose(delegate.file);
            delegate.file = nullptr;
        }

        if (request.m_body != nullptr && delegate.body != nil) {
            request.m_body->assign((const char*)delegate.body.bytes, delegate.body.length);
        }

        result.m_bytes_written = delegate.received;
        result.m_http_code = delegate.httpCode;

        if (delegate.cancelledByCaller) {
            result.m_status = HttpStatus::Cancelled;
            result.m_message = "Cancelled.";
        } else if (delegate.writeFailed) {
            result.m_status = HttpStatus::FileError;
            result.m_message = "Could not write the received data.";
        } else if (delegate.bodyTooLarge) {
            result.m_status = HttpStatus::HttpError;
            result.m_message = "Response body exceeded the caller's limit.";
        } else if (delegate.httpCode > 0 && (delegate.httpCode < 200 || delegate.httpCode >= 300)) {
            result.m_status = HttpStatus::HttpError;
            result.m_message = "Server returned HTTP " + std::to_string(delegate.httpCode);
        } else if (delegate.failure != nil) {
            result.m_status = HttpStatus::NetworkError;
            result.m_message = delegate.failure.localizedDescription.UTF8String;
        } else {
            result.m_status = HttpStatus::Ok;
        }

        return result;
    }
}

bool backend_supported() {
    return true;
}

}  // namespace thl::net::detail
