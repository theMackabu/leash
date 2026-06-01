#import "leash/run/backend.h"

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

@interface OccurrenceCounter : NSObject
- (instancetype)initWithPattern:(NSData *)pattern;
- (NSUInteger)processData:(NSData *)data;
@end

@interface VMDelegate : NSObject <VZVirtualMachineDelegate>
@end

static struct termios original_stdin_term;
static BOOL has_original_stdin_term = NO;
static VZVirtualMachine *global_vm = nil;
static VMDelegate *global_delegate = nil;
static NSMutableArray *signal_sources = nil;

static NSString *nsstr(const char *s) {
  return s ? [NSString stringWithUTF8String:s] : nil;
}

static void write_stderr(NSString *message) {
  NSData *data = [message dataUsingEncoding:NSUTF8StringEncoding];
  if (data) [[NSFileHandle fileHandleWithStandardError] writeData:data];
}

static void reset_tty(void) {
  if (has_original_stdin_term) tcsetattr(STDIN_FILENO, TCSANOW, &original_stdin_term);
}

static void quit_process(int code) __attribute__((noreturn));
static void quit_process(int code) {
  reset_tty();
  exit(code);
}

static void fail_ns(NSString *message) __attribute__((noreturn));
static void fail_ns(NSString *message) {
  write_stderr([message stringByAppendingString:@"\n"]);
  exit(1);
}

static void setup_tty(void) {
  if (isatty(STDIN_FILENO) != 0) {
    struct termios term;
    tcgetattr(STDIN_FILENO, &original_stdin_term);
    tcgetattr(STDIN_FILENO, &term);
    cfmakeraw(&term);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    has_original_stdin_term = YES;
  }
}

static void request_graceful_stop(double timeout) {
  if (global_vm && global_vm.canRequestStop) {
    NSError *error = nil;
    if ([global_vm requestStopWithError:&error]) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        write_stderr(@"Shutdown timeout expired, exiting immediately.\r\n");
        quit_process(1);
      });
      return;
    }
    write_stderr(@"Failed to request stop.\r\n");
  }
  quit_process(1);
}

static VZVirtioBlockDeviceConfiguration *make_disk(NSString *path, BOOL read_only) {
  NSError *error = nil;
  VZDiskImageStorageDeviceAttachment *attachment =
    [[VZDiskImageStorageDeviceAttachment alloc] initWithURL:[NSURL fileURLWithPath:path]
                                                   readOnly:read_only
                                                      error:&error];
  if (!attachment) fail_ns([NSString stringWithFormat:@"Failed to open %@: %@", path, error.localizedDescription]);
  return [[VZVirtioBlockDeviceConfiguration alloc] initWithAttachment:attachment];
}

static void configure_boot(VZVirtualMachineConfiguration *config, const vm_run_options *options) {
  switch (options->bootloader) {
  case VM_BOOT_LINUX: {
    if (!options->kernel) fail_ns(@"Kernel not specified");
    if (options->efi_vars) fail_ns(@"EFI variable store cannot be used with Linux bootloader");
    VZLinuxBootLoader *loader =
      [[VZLinuxBootLoader alloc] initWithKernelURL:[NSURL fileURLWithPath:nsstr(options->kernel)]];
    if (options->initrd) loader.initialRamdiskURL = [NSURL fileURLWithPath:nsstr(options->initrd)];
    if (options->cmdline) loader.commandLine = nsstr(options->cmdline);
    config.bootLoader = loader;
    break;
  }
  case VM_BOOT_EFI:
    if (@available(macOS 13.0, *)) {
      if (!options->efi_vars) fail_ns(@"EFI variable store must be specified if using EFI bootloader");
      if (options->kernel || options->initrd || options->cmdline) {
        fail_ns(@"Kernel, initrd and cmdline options cannot be used with EFI bootloader");
      }
      NSString *vars = nsstr(options->efi_vars);
      NSURL *url = [NSURL fileURLWithPath:vars];
      VZEFIVariableStore *store = nil;
      if ([[NSFileManager defaultManager] fileExistsAtPath:vars]) {
        store = [[VZEFIVariableStore alloc] initWithURL:url];
      } else {
        NSError *error = nil;
        store = [[VZEFIVariableStore alloc] initCreatingVariableStoreAtURL:url options:0 error:&error];
        if (!store)
          fail_ns([NSString stringWithFormat:@"Failed to create EFI variable store: %@", error.localizedDescription]);
      }
      VZEFIBootLoader *loader = [[VZEFIBootLoader alloc] init];
      loader.variableStore = store;
      config.bootLoader = loader;
    } else {
      fail_ns(@"EFI bootloader is only available on macOS 13 and later versions");
    }
  }
}

static void configure_storage(VZVirtualMachineConfiguration *config, const vm_run_options *options) {
  NSMutableArray *devices = [NSMutableArray array];
  for (size_t i = 0; i < options->disks.count; i++)
    [devices addObject:make_disk(nsstr(options->disks.items[i]), NO)];
  for (size_t i = 0; i < options->cdroms.count; i++)
    [devices addObject:make_disk(nsstr(options->cdroms.items[i]), YES)];
  config.storageDevices = devices;
}

static void configure_folders(VZVirtualMachineConfiguration *config, const vm_run_options *options) {
  if (@available(macOS 12.0, *)) {
    NSMutableArray *devices = [NSMutableArray array];
    for (size_t i = 0; i < options->folders.count; i++) {
      NSString *folder = nsstr(options->folders.items[i]);
      NSArray<NSString *> *parts = [folder componentsSeparatedByString:@":"];
      if (parts.count > 3) fail_ns([NSString stringWithFormat:@"Too many components in shared folder: %@", folder]);
      NSString *path = parts[0];
      NSString *tag = parts.count > 1 ? parts[1] : path;
      BOOL read_only = parts.count > 2 && [parts[2] isEqualToString:@"ro"];
      printf("Adding shared folder '%s' with tag %s, but be warned, this might be unstable.\n", path.UTF8String,
             tag.UTF8String);
      VZSharedDirectory *shared = [[VZSharedDirectory alloc] initWithURL:[NSURL fileURLWithPath:path]
                                                                readOnly:read_only];
      VZVirtioFileSystemDeviceConfiguration *fs = [[VZVirtioFileSystemDeviceConfiguration alloc] initWithTag:tag];
      fs.share = [[VZSingleDirectoryShare alloc] initWithDirectory:shared];
      [devices addObject:fs];
    }
    if (@available(macOS 13.0, *)) {
#if defined(__arm64__)
      NSError *error = nil;
      if ([VZVirtioFileSystemDeviceConfiguration validateTag:@"rosetta" error:&error]) {
        error = nil;
        VZLinuxRosettaDirectoryShare *share = [[VZLinuxRosettaDirectoryShare alloc] initWithError:&error];
        if (share) {
          VZVirtioFileSystemDeviceConfiguration *fs =
            [[VZVirtioFileSystemDeviceConfiguration alloc] initWithTag:@"rosetta"];
          fs.share = share;
          [devices addObject:fs];
        }
      }
#endif
    }
    config.directorySharingDevices = devices;
  }
}

static void configure_networks(VZVirtualMachineConfiguration *config, const vm_run_options *options) {
  NSMutableArray *devices = [NSMutableArray array];
  for (size_t i = 0; i < options->networks.count; i++) {
    NSString *network = nsstr(options->networks.items[i]);
    NSArray<NSString *> *parts = [network componentsSeparatedByString:@"@"];
    NSString *device = parts[0];
    VZVirtioNetworkDeviceConfiguration *net = [[VZVirtioNetworkDeviceConfiguration alloc] init];
    if (parts.count > 1) {
      VZMACAddress *mac = [[VZMACAddress alloc] initWithString:parts[0]];
      if (!mac) fail_ns([NSString stringWithFormat:@"Invalid MAC address: %@", parts[0]]);
      net.MACAddress = mac;
      device = parts[1];
    }
    if ([device isEqualToString:@"nat"]) {
      net.attachment = [[VZNATNetworkDeviceAttachment alloc] init];
    } else {
      for (VZBridgedNetworkInterface *iface in VZBridgedNetworkInterface.networkInterfaces) {
        if ([iface.identifier isEqualToString:device]) {
          net.attachment = [[VZBridgedNetworkDeviceAttachment alloc] initWithInterface:iface];
          break;
        }
      }
      if (!net.attachment) fail_ns([NSString stringWithFormat:@"Cannot find network: %@", network]);
    }
    [devices addObject:net];
  }
  config.networkDevices = devices;
}

static void install_signals(void) {
  signal_sources = [NSMutableArray array];
  int signals[] = {SIGPIPE, SIGHUP, SIGINT, SIGTERM};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    signal(signals[i], SIG_IGN);
    dispatch_source_t source =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signals[i], 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{ quit_process(1); });
    dispatch_resume(source);
    [signal_sources addObject:source];
  }
}

@implementation OccurrenceCounter {
  NSData *_pattern;
  NSUInteger _index;
}
- (instancetype)initWithPattern:(NSData *)pattern {
  self = [super init];
  if (self) _pattern = pattern;
  return self;
}
- (NSUInteger)processData:(NSData *)data {
  if (_pattern.length == 0) return 0;
  const uint8_t *p = _pattern.bytes;
  const uint8_t *d = data.bytes;
  NSUInteger hits = 0;
  for (NSUInteger i = 0; i < data.length; i++) {
    if (d[i] == p[_index]) {
      if (++_index >= _pattern.length) {
        hits++;
        _index = 0;
      }
    } else {
      _index = 0;
    }
  }
  return hits;
}
@end

@implementation VMDelegate
- (void)guestDidStopVirtualMachine:(VZVirtualMachine *)virtualMachine {
  (void)virtualMachine;
  quit_process(0);
}
- (void)virtualMachine:(VZVirtualMachine *)virtualMachine didStopWithError:(NSError *)error {
  (void)virtualMachine;
  (void)error;
  quit_process(1);
}
@end

int vm_run_backend(const vm_run_options *options) {
  @autoreleasepool {
    install_signals();

    VZVirtualMachineConfiguration *config = [[VZVirtualMachineConfiguration alloc] init];
    config.CPUCount = options->cpu_count;
    config.memorySize = options->memory_size * options->memory_size_suffix;
    configure_boot(config, options);

    NSPipe *serial_in = [NSPipe pipe];
    NSPipe *serial_out = [NSPipe pipe];
    NSFileHandle *serial_in_writer = serial_in.fileHandleForWriting;
    NSFileHandle *serial_out_reader = serial_out.fileHandleForReading;
    VZVirtioConsoleDeviceSerialPortConfiguration *console = [[VZVirtioConsoleDeviceSerialPortConfiguration alloc] init];
    console.attachment =
      [[VZFileHandleSerialPortAttachment alloc] initWithFileHandleForReading:serial_in.fileHandleForReading
                                                        fileHandleForWriting:serial_out.fileHandleForWriting];
    config.serialPorts = @[ console ];

    configure_storage(config, options);
    configure_folders(config, options);
    configure_networks(config, options);
    (void)options->balloon;
    config.memoryBalloonDevices = @[ [[VZVirtioTraditionalMemoryBalloonDeviceConfiguration alloc] init] ];
    config.entropyDevices = @[ [[VZVirtioEntropyDeviceConfiguration alloc] init] ];

    NSError *error = nil;
    if (![config validateWithError:&error]) fail_ns(error.localizedDescription);

    setup_tty();
    NSMutableData *escape = [NSMutableData dataWithBytes:"\033" length:1];
    NSData *suffix = [nsstr(options->escape_sequence) dataUsingEncoding:NSNonLossyASCIIStringEncoding];
    if (suffix) [escape appendData:suffix];
    OccurrenceCounter *counter = [[OccurrenceCounter alloc] initWithPattern:escape];

    NSFileHandle *stdin_handle = [NSFileHandle fileHandleWithStandardInput];
    [stdin_handle waitForDataInBackgroundAndNotify];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSFileHandleDataAvailableNotification
                                                      object:stdin_handle
                                                       queue:nil
                                                  usingBlock:^(__unused NSNotification *note) {
                                                    NSData *data = stdin_handle.availableData;
                                                    if (has_original_stdin_term && [counter processData:data] > 0) {
                                                      write_stderr(@"Escape sequence detected, exiting.\r\n");
                                                      request_graceful_stop(options->shutdown_timeout);
                                                    }
                                                    [serial_in_writer writeData:data];
                                                    if (data.length > 0)
                                                      [stdin_handle waitForDataInBackgroundAndNotify];
                                                  }];

    [serial_out_reader waitForDataInBackgroundAndNotify];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSFileHandleDataAvailableNotification
                                                      object:serial_out_reader
                                                       queue:nil
                                                  usingBlock:^(__unused NSNotification *note) {
                                                    NSData *data = serial_out_reader.availableData;
                                                    [[NSFileHandle fileHandleWithStandardOutput] writeData:data];
                                                    if (data.length > 0)
                                                      [serial_out_reader waitForDataInBackgroundAndNotify];
                                                  }];

    dispatch_source_set_event_handler(signal_sources[2], ^{ request_graceful_stop(options->shutdown_timeout); });
    dispatch_source_set_event_handler(signal_sources[3], ^{ request_graceful_stop(options->shutdown_timeout); });

    global_vm = [[VZVirtualMachine alloc] initWithConfiguration:config];
    global_delegate = [[VMDelegate alloc] init];
    global_vm.delegate = global_delegate;
    [global_vm startWithCompletionHandler:^(NSError *_Nullable start_error) {
      if (start_error) {
        write_stderr([start_error.localizedDescription stringByAppendingString:@"\n"]);
        quit_process(1);
      }
    }];
    [[NSRunLoop mainRunLoop] run];
  }
  return 0;
}
