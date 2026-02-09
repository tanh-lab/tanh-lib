#import "ViewController.h"
#import <tanh/audio_io.h>
#import <tanh/audio_io/AudioFileSink.h>
#import <tanh/audio_io/AudioPlayerSource.h>
#import <memory>

@interface ViewController ()
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIButton *startAudioButton;
@property(nonatomic, strong) UIButton *stopAudioButton;
@property(nonatomic, strong) UIButton *recordButton;
@property(nonatomic, strong) UIButton *playButton;
@property(nonatomic, strong) UITextView *logTextView;
@property(nonatomic, copy) NSString *recordingPath;
@property(nonatomic, assign) BOOL hasRecording;
@end

@implementation ViewController {
    std::unique_ptr<thl::AudioDeviceManager> _audioManager;
    std::unique_ptr<thl::AudioFileSink> _fileSink;
    std::unique_ptr<thl::AudioPlayerSource> _playerSource;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.view.backgroundColor = [UIColor whiteColor];
    self.recordingPath = [NSTemporaryDirectory()
        stringByAppendingPathComponent:@"recording.wav"];
    _hasRecording = NO;

    [self setupUI];
    [self initializeAudio];
}

- (void)setupUI {
    // Status label
    _statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 0, 0)];
    _statusLabel.text = @"Audio IO Recording Test";
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.font = [UIFont boldSystemFontOfSize:20];
    [self.view addSubview:_statusLabel];

    // Start Audio button
    _startAudioButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_startAudioButton setTitle:@"Start Audio" forState:UIControlStateNormal];
    [_startAudioButton setTitleColor:[UIColor whiteColor]
                            forState:UIControlStateNormal];
    [_startAudioButton addTarget:self
                          action:@selector(startAudio)
                forControlEvents:UIControlEventTouchUpInside];
    _startAudioButton.backgroundColor = [UIColor colorWithRed:0.2
                                                        green:0.6
                                                         blue:0.2
                                                        alpha:1.0];
    _startAudioButton.layer.cornerRadius = 8;
    [self.view addSubview:_startAudioButton];

    // Stop Audio button
    _stopAudioButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_stopAudioButton setTitle:@"Stop Audio" forState:UIControlStateNormal];
    [_stopAudioButton setTitleColor:[UIColor whiteColor]
                           forState:UIControlStateNormal];
    [_stopAudioButton addTarget:self
                         action:@selector(stopAudio)
               forControlEvents:UIControlEventTouchUpInside];
    _stopAudioButton.backgroundColor = [UIColor colorWithRed:0.6
                                                       green:0.2
                                                        blue:0.2
                                                       alpha:1.0];
    _stopAudioButton.layer.cornerRadius = 8;
    _stopAudioButton.enabled = NO;
    _stopAudioButton.alpha = 0.5;
    [self.view addSubview:_stopAudioButton];

    // Record toggle button
    _recordButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_recordButton setTitle:@"Start Recording" forState:UIControlStateNormal];
    [_recordButton setTitleColor:[UIColor whiteColor]
                        forState:UIControlStateNormal];
    [_recordButton addTarget:self
                      action:@selector(toggleRecording)
            forControlEvents:UIControlEventTouchUpInside];
    _recordButton.backgroundColor = [UIColor colorWithRed:0.8
                                                    green:0.4
                                                     blue:0.0
                                                    alpha:1.0];
    _recordButton.layer.cornerRadius = 8;
    _recordButton.enabled = NO;
    _recordButton.alpha = 0.5;
    [self.view addSubview:_recordButton];

    // Play Recording button
    _playButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_playButton setTitle:@"Play Recording" forState:UIControlStateNormal];
    [_playButton setTitleColor:[UIColor whiteColor]
                      forState:UIControlStateNormal];
    [_playButton addTarget:self
                    action:@selector(playRecording)
          forControlEvents:UIControlEventTouchUpInside];
    _playButton.backgroundColor = [UIColor colorWithRed:0.2
                                                  green:0.4
                                                   blue:0.8
                                                  alpha:1.0];
    _playButton.layer.cornerRadius = 8;
    _playButton.enabled = NO;
    _playButton.alpha = 0.5;
    [self.view addSubview:_playButton];

    // Log text view
    _logTextView = [[UITextView alloc] initWithFrame:CGRectMake(0, 0, 0, 0)];
    _logTextView.editable = NO;
    _logTextView.font = [UIFont fontWithName:@"Courier" size:12];
    _logTextView.layer.borderWidth = 1;
    _logTextView.layer.borderColor = [UIColor grayColor].CGColor;
    [self.view addSubview:_logTextView];
}

- (void)viewWillLayoutSubviews {
    [super viewWillLayoutSubviews];

    CGFloat viewWidth = self.view.bounds.size.width;
    CGFloat viewHeight = self.view.bounds.size.height;
    CGFloat buttonWidth = (viewWidth - 60) / 2;
    CGFloat safeTop = self.view.safeAreaInsets.top;
    CGFloat safeBottom = self.view.safeAreaInsets.bottom;
    CGFloat yPos = safeTop + 20;

    _statusLabel.frame = CGRectMake(20, yPos, viewWidth - 40, 40);
    yPos += 50;

    _startAudioButton.frame = CGRectMake(20, yPos, buttonWidth, 50);
    _stopAudioButton.frame = CGRectMake(40 + buttonWidth, yPos, buttonWidth, 50);
    yPos += 60;

    _recordButton.frame = CGRectMake(20, yPos, buttonWidth, 50);
    _playButton.frame = CGRectMake(40 + buttonWidth, yPos, buttonWidth, 50);
    yPos += 70;

    _logTextView.frame = CGRectMake(20, yPos, viewWidth - 40,
                                    viewHeight - yPos - safeBottom - 20);
}

- (void)initializeAudio {
    [self log:@"Initialising AudioDeviceManager..."];

    _audioManager = std::make_unique<thl::AudioDeviceManager>();

    if (!_audioManager->isContextInitialised()) {
        [self log:@"ERROR: Failed to initialise audio context"];
        return;
    }

    [self log:@"Audio context initialised successfully"];

    // Enumerate devices
    auto inputDevices = _audioManager->enumerateInputDevices();
    auto outputDevices = _audioManager->enumerateOutputDevices();

    [self log:[NSString stringWithFormat:@"Found %lu input devices",
                                         inputDevices.size()]];
    for (size_t i = 0; i < inputDevices.size(); ++i) {
        [self log:[NSString stringWithFormat:@"  Input %zu: %s",
                                             i,
                                             inputDevices[i].name.c_str()]];
    }

    [self log:[NSString stringWithFormat:@"Found %lu output devices",
                                         outputDevices.size()]];
    for (size_t i = 0; i < outputDevices.size(); ++i) {
        [self log:[NSString stringWithFormat:@"  Output %zu: %s",
                                             i,
                                             outputDevices[i].name.c_str()]];
    }

    // Initialise with both input and output (separate devices)
    if (!inputDevices.empty() && !outputDevices.empty()) {
        bool success = _audioManager->initialise(&inputDevices[0],
                                                 &outputDevices[0],
                                                 48000,
                                                 512,
                                                 1,   // mono input
                                                 1);  // mono output (must match for playback)

        if (success) {
            [self log:@"Audio device initialised"];
            [self
                log:[NSString stringWithFormat:@"Sample Rate: %u",
                                               _audioManager->getSampleRate()]];
            [self
                log:[NSString stringWithFormat:@"Buffer Size: %u",
                                               _audioManager->getBufferSize()]];
            [self
                log:[NSString
                        stringWithFormat:@"Input Channels: %u",
                                         _audioManager->getNumInputChannels()]];
            [self log:[NSString stringWithFormat:@"Output Channels: %u",
                                                 _audioManager
                                                     ->getNumOutputChannels()]];

            // Create AudioFileSink for recording
            _fileSink = std::make_unique<thl::AudioFileSink>();
            _audioManager->addCaptureCallback(_fileSink.get());
            [self log:@"AudioFileSink registered for capture"];

            // Create AudioPlayerSource for playback
            _playerSource = std::make_unique<thl::AudioPlayerSource>();
            _audioManager->addPlaybackCallback(_playerSource.get());
            [self log:@"AudioPlayerSource registered for playback"];

            // Set up finished callback (safe because _playerSource is destroyed
            // before self in dealloc)
            ViewController *controller = self;
            _playerSource->setFinishedCallback([controller]() {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [controller onPlaybackFinished];
                });
            });

        } else {
            [self log:@"ERROR: Failed to initialise audio device"];
        }
    } else {
        [self log:@"ERROR: Need both input and output devices for recording"];
    }
}

- (void)startAudio {
    [self log:@"Starting audio..."];

    bool playbackStarted = false;

    if (_audioManager) {
        playbackStarted = _audioManager->startPlayback();
    }

    if (playbackStarted) {
        [self log:@"Audio started (playback only)"];
        _statusLabel.text = @"Audio Running";
        _startAudioButton.enabled = NO;
        _startAudioButton.alpha = 0.5;
        _stopAudioButton.enabled = YES;
        _stopAudioButton.alpha = 1.0;
        _recordButton.enabled = YES;
        _recordButton.alpha = 1.0;
        [self updatePlayButtonState];
    } else {
        [self log:@"ERROR: Failed to start playback"];
    }
}

- (void)stopAudio {
    [self log:@"Stopping audio..."];

    if (_fileSink && _fileSink->isRecording()) {
        _fileSink->stopRecording();
        _fileSink->closeFile();
    }

    if (_playerSource && _playerSource->isPlaying()) {
        _playerSource->stop();
    }

    if (_audioManager) {
        _audioManager->stopCapture();
        _audioManager->stopPlayback();
        [self log:@"Audio stopped"];
        _statusLabel.text = @"Audio Stopped";
        _startAudioButton.enabled = YES;
        _startAudioButton.alpha = 1.0;
        _stopAudioButton.enabled = NO;
        _stopAudioButton.alpha = 0.5;
        _recordButton.enabled = NO;
        _recordButton.alpha = 0.5;
        [_recordButton setTitle:@"Start Recording"
                       forState:UIControlStateNormal];
        _playButton.enabled = NO;
        _playButton.alpha = 0.5;
    }
}

- (void)toggleRecording {
    if (!_fileSink) return;

    if (_fileSink->isRecording()) {
        // Stop recording
        _fileSink->stopRecording();
        _fileSink->closeFile();

        // Stop capture — only needed while recording
        _audioManager->stopCapture();
        [self log:@"Capture stopped"];

        uint64_t framesWritten = _fileSink->getFramesWritten();
        float duration =
            static_cast<float>(framesWritten) / _audioManager->getSampleRate();

        [_recordButton setTitle:@"Start Recording"
                       forState:UIControlStateNormal];
        _recordButton.backgroundColor = [UIColor colorWithRed:0.8
                                                        green:0.4
                                                         blue:0.0
                                                        alpha:1.0];
        [self log:[NSString stringWithFormat:@"Recording stopped (%.2fs)",
                                             duration]];
        _hasRecording = YES;
        [self updatePlayButtonState];
    } else {
        // Stop any ongoing playback first
        if (_playerSource && _playerSource->isPlaying()) {
            _playerSource->stop();
            _playerSource->unloadFile();
        }

        // Start capture for the recording
        if (!_audioManager->startCapture()) {
            [self log:@"ERROR: Failed to start capture"];
            return;
        }
        [self log:@"Capture started"];

        // Open file and start recording
        bool opened = _fileSink->openFile(
            [_recordingPath UTF8String],
            _audioManager->getNumInputChannels(),
            _audioManager->getSampleRate(),
            thl::AudioEncodingFormat::WAV);

        if (opened) {
            _fileSink->startRecording();
            [_recordButton setTitle:@"Stop Recording"
                           forState:UIControlStateNormal];
            _recordButton.backgroundColor = [UIColor colorWithRed:0.9
                                                            green:0.1
                                                             blue:0.1
                                                            alpha:1.0];
            [self log:@"Recording started..."];
            [self log:[NSString stringWithFormat:@"File: %@", _recordingPath]];
            _playButton.enabled = NO;
            _playButton.alpha = 0.5;
        } else {
            _audioManager->stopCapture();
            [self log:@"ERROR: Failed to open file for recording"];
        }
    }
}

- (void)playRecording {
    if (!_playerSource || !_hasRecording) return;

    // Cannot play while recording
    if (_fileSink && _fileSink->isRecording()) {
        [self log:@"Cannot play while recording"];
        return;
    }

    // Load and play the recording (mono file, mono output)
    bool loaded = _playerSource->loadFile([_recordingPath UTF8String],
                                          1,  // mono - must match recorded channels
                                          _audioManager->getSampleRate());

    if (loaded) {
        _playerSource->play();

        uint64_t totalFrames = _playerSource->getTotalFrames();
        float duration =
            static_cast<float>(totalFrames) / _audioManager->getSampleRate();

        [self log:[NSString stringWithFormat:@"Playing recording (%.2fs)...",
                                             duration]];
        _playButton.enabled = NO;
        _playButton.alpha = 0.5;
        _recordButton.enabled = NO;
        _recordButton.alpha = 0.5;
    } else {
        [self log:@"ERROR: Failed to load recording for playback"];
    }
}

- (void)onPlaybackFinished {
    [self log:@"Playback finished"];

    if (_playerSource) {
        _playerSource->unloadFile();
    }

    // Re-enable buttons
    if (_audioManager && _audioManager->isPlaybackRunning()) {
        _recordButton.enabled = YES;
        _recordButton.alpha = 1.0;
        [self updatePlayButtonState];
    }
}

- (void)updatePlayButtonState {
    BOOL canPlay = _hasRecording && _audioManager &&
                   _audioManager->isPlaybackRunning() &&
                   (!_fileSink || !_fileSink->isRecording()) &&
                   (!_playerSource || !_playerSource->isPlaying());

    _playButton.enabled = canPlay;
    _playButton.alpha = canPlay ? 1.0 : 0.5;
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
    if (_fileSink) {
        _fileSink->stopRecording();
        _fileSink->closeFile();
    }
    if (_playerSource) {
        _playerSource->setFinishedCallback(nullptr);
        _playerSource->stop();
        _playerSource->unloadFile();
    }
    if (_audioManager) {
        _audioManager->stopCapture();
        _audioManager->stopPlayback();
        _audioManager->shutdown();
    }
    [_recordingPath release];
    [_statusLabel release];
    [_startAudioButton release];
    [_stopAudioButton release];
    [_recordButton release];
    [_playButton release];
    [_logTextView release];
    [super dealloc];
}

@end
