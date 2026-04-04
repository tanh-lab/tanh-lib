#import "AppDelegate.h"
#import "ViewController.h"
#import <tanh/core/Logger.h>

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    auto config = thl::Logger::get_config();
    config.m_console_enabled = true;
    thl::Logger::set_config(config);

    _window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    _window.rootViewController = [[ViewController alloc] init];
    [_window makeKeyAndVisible];
    return YES;
}

@end
