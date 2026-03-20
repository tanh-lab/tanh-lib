#import "AppDelegate.h"
#import "ViewController.h"
#import <tanh/core/Logger.h>
#import <iostream>

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    thl::Logger::set_callback([](const thl::Logger::LogRecord& record) {
        auto level = static_cast<thl::Logger::LogLevel>(record.level);
        std::string line = thl::Logger::format_plain(record);
        if (level <= thl::Logger::LogLevel::Warning) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
    });

    _window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    _window.rootViewController = [[ViewController alloc] init];
    [_window makeKeyAndVisible];
    return YES;
}

@end
