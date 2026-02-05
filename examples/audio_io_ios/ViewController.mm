#import "ViewController.h"
#import <tanh/audio_io.h>
#import <memory>

// Simple callback that just outputs silence or passes through input
class SimpleAudioCallback : public thl::AudioIODeviceCallback {
public:
    void process(float *outputBuffer,
                 const float *inputBuffer,
                 ma_uint32 frameCount,
                 ma_uint32 numInputChannels,
                 ma_uint32 numOutputChannels) override {
        // Just output silence for now (zero out the buffer)
        if (outputBuffer && numOutputChannels > 0) {
            size_t totalSamples = frameCount * numOutputChannels;
            for (size_t i = 0; i < totalSamples; ++i) {
                outputBuffer[i] = 0.0f;
            }
        }

        // Log every 100 callbacks to verify audio is running
        static int callbackCount = 0;
        if (++callbackCount % 100 == 0) {
            printf("Audio callback count: %d, frames: %u, channels: %u\n",
                   callbackCount,
                   frameCount,
                   numOutputChannels);
        }
        (void)inputBuffer;
        (void)numInputChannels;
    }

    void prepareToPlay(ma_uint32 sampleRate, ma_uint32 bufferSize) override {
        printf("PrepareToPlay called: SR=%u, Buffer=%u\n",
               sampleRate,
               bufferSize);
    }

    void releaseResources() override { printf("ReleaseResources called\n"); }
};

@interface ViewController ()
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIButton *startButton;
@property(nonatomic, strong) UIButton *stopButton;
@property(nonatomic, strong) UITextView *logTextView;
@end

@implementation ViewController {
    std::unique_ptr<thl::AudioDeviceManager> _audioManager;
    std::unique_ptr<SimpleAudioCallback> _callback;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.view.backgroundColor = [UIColor whiteColor];

    // Create UI elements
    [self setupUI];

    // Initialize audio manager
    [self initializeAudio];
}

- (void)setupUI {
    // Status label at the top
    _statusLabel = [[UILabel alloc]
        initWithFrame:CGRectMake(20,
                                 100,
                                 self.view.bounds.size.width - 40,
                                 40)];
    _statusLabel.text = @"Audio IO Test";
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.font = [UIFont boldSystemFontOfSize:20];
    [self.view addSubview:_statusLabel];

    // Start button
    _startButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _startButton.frame =
        CGRectMake(20, 160, self.view.bounds.size.width - 40, 50);
    [_startButton setTitle:@"Start Audio" forState:UIControlStateNormal];
    [_startButton addTarget:self
                     action:@selector(startAudio)
           forControlEvents:UIControlEventTouchUpInside];
    _startButton.backgroundColor = [UIColor colorWithRed:0.2
                                                   green:0.8
                                                    blue:0.2
                                                   alpha:1.0];
    _startButton.layer.cornerRadius = 8;
    [self.view addSubview:_startButton];

    // Stop button
    _stopButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _stopButton.frame =
        CGRectMake(20, 220, self.view.bounds.size.width - 40, 50);
    [_stopButton setTitle:@"Stop Audio" forState:UIControlStateNormal];
    [_stopButton addTarget:self
                    action:@selector(stopAudio)
          forControlEvents:UIControlEventTouchUpInside];
    _stopButton.backgroundColor = [UIColor colorWithRed:0.8
                                                  green:0.2
                                                   blue:0.2
                                                  alpha:1.0];
    _stopButton.layer.cornerRadius = 8;
    _stopButton.enabled = NO;
    [self.view addSubview:_stopButton];

    // Log text view
    _logTextView = [[UITextView alloc]
        initWithFrame:CGRectMake(20,
                                 290,
                                 self.view.bounds.size.width - 40,
                                 self.view.bounds.size.height - 320)];
    _logTextView.editable = NO;
    _logTextView.font = [UIFont fontWithName:@"Courier" size:12];
    _logTextView.layer.borderWidth = 1;
    _logTextView.layer.borderColor = [UIColor grayColor].CGColor;
    [self.view addSubview:_logTextView];
}

- (void)initializeAudio {
    [self log:@"Initializing AudioDeviceManager..."];

    // Create audio manager
    _audioManager = std::make_unique<thl::AudioDeviceManager>();

    if (!_audioManager->isContextInitialised()) {
        [self log:@"ERROR: Failed to initialize audio context"];
        return;
    }

    [self log:@"Audio context initialized successfully"];

    // Enumerate devices
    auto outputDevices = _audioManager->enumerateOutputDevices();
    [self log:[NSString stringWithFormat:@"Found %lu output devices",
                                         outputDevices.size()]];

    for (size_t i = 0; i < outputDevices.size(); ++i) {
        [self log:[NSString stringWithFormat:@"  Device %zu: %s",
                                             i,
                                             outputDevices[i].name.c_str()]];
    }

    // Initialize with default output device
    if (!outputDevices.empty()) {
        bool success =
            _audioManager->initialise(nullptr,            // no input
                                      &outputDevices[0],  // default output
                                      48000,             // sample rate
                                      512,               // buffer size
                                      0,                 // no input channels
                                      2                  // stereo output
            );

        if (success) {
            [self log:@"Audio device initialized successfully"];
            [self
                log:[NSString stringWithFormat:@"Sample Rate: %u",
                                               _audioManager->getSampleRate()]];
            [self
                log:[NSString stringWithFormat:@"Buffer Size: %u",
                                               _audioManager->getBufferSize()]];
            [self log:[NSString
                          stringWithFormat:@"Output Channels: %u",
                                           _audioManager->getNumOutputChannels()]];
            [self log:[NSString
                          stringWithFormat:@"Input Channels: %u",
                                           _audioManager->getNumInputChannels()]];

            // Create and add callback
            _callback = std::make_unique<SimpleAudioCallback>();
            _audioManager->addCallback(_callback.get());
            [self log:@"Audio callback registered"];
        } else {
            [self log:@"ERROR: Failed to initialize audio device"];
        }
    } else {
        [self log:@"ERROR: No output devices found"];
    }
}

- (void)startAudio {
    [self log:@"Starting audio..."];

    if (_audioManager && _audioManager->start()) {
        [self log:@"Audio started successfully"];
        _statusLabel.text = @"Audio Running";
        _startButton.enabled = NO;
        _stopButton.enabled = YES;
    } else {
        [self log:@"ERROR: Failed to start audio"];
    }
}

- (void)stopAudio {
    [self log:@"Stopping audio..."];

    if (_audioManager) {
        _audioManager->stop();
        [self log:@"Audio stopped"];
        _statusLabel.text = @"Audio Stopped";
        _startButton.enabled = YES;
        _stopButton.enabled = NO;
    }
}

- (void)log:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString *timestamp = [NSDateFormatter
            localizedStringFromDate:[NSDate date]
                          dateStyle:NSDateFormatterNoStyle
                          timeStyle:NSDateFormatterMediumStyle];
        NSString *logMessage =
            [NSString stringWithFormat:@"[%@] %@\n", timestamp, message];

        self.logTextView.text =
            [self.logTextView.text stringByAppendingString:logMessage];

        // Scroll to bottom
        NSRange bottom = NSMakeRange(self.logTextView.text.length - 1, 1);
        [self.logTextView scrollRangeToVisible:bottom];
    });
}

- (void)dealloc {
    if (_audioManager) {
        _audioManager->stop();
        _audioManager->shutdown();
    }
}

@end
