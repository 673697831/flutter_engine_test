// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/shell/platform/darwin/ios/framework/Source/FlutterDartProject_Internal.h"

#include "flutter/common/task_runners.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/platform/darwin/scoped_nsobject.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/shell.h"
#include "flutter/shell/common/switches.h"
#include "flutter/shell/platform/darwin/common/command_line.h"
#include "flutter/shell/platform/darwin/ios/framework/Headers/FlutterViewController.h"

extern "C" {
#if FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG
// Used for debugging dart:* sources.
extern const uint8_t kPlatformStrongDill[];
extern const intptr_t kPlatformStrongDillSize;
#endif
}

static const char* kApplicationKernelSnapshotFileName = "kernel_blob.bin";

static flutter::Settings DefaultSettingsForProcess(NSBundle* bundle = nil) {
  auto command_line = flutter::CommandLineFromNSProcessInfo();

  // Precedence:
  // 1. Settings from the specified NSBundle.
  // 2. Settings passed explicitly via command-line arguments.
  // 3. Settings from the NSBundle with the default bundle ID.
  // 4. Settings from the main NSBundle and default values.

  NSBundle* mainBundle = [NSBundle mainBundle];
  NSBundle* engineBundle = [NSBundle bundleForClass:[FlutterViewController class]];

  bool hasExplicitBundle = bundle != nil;
  if (bundle == nil) {
    bundle = [NSBundle bundleWithIdentifier:[FlutterDartProject defaultBundleIdentifier]];
  }
  if (bundle == nil) {
    bundle = mainBundle;
  }

  auto settings = flutter::SettingsFromCommandLine(command_line);

  settings.task_observer_add = [](intptr_t key, fml::closure callback) {
    fml::MessageLoop::GetCurrent().AddTaskObserver(key, std::move(callback));
  };

  settings.task_observer_remove = [](intptr_t key) {
    fml::MessageLoop::GetCurrent().RemoveTaskObserver(key);
  };

  // The command line arguments may not always be complete. If they aren't, attempt to fill in
  // defaults.

  // Flutter ships the ICU data file in the the bundle of the engine. Look for it there.
  if (settings.icu_data_path.size() == 0) {
    NSString* icuDataPath = [engineBundle pathForResource:@"icudtl" ofType:@"dat"];
    if (icuDataPath.length > 0) {
      settings.icu_data_path = icuDataPath.UTF8String;
    }
  }

  if (flutter::DartVM::IsRunningPrecompiledCode()) {
    if (hasExplicitBundle) {
      NSString* executablePath = bundle.executablePath;
      if ([[NSFileManager defaultManager] fileExistsAtPath:executablePath]) {
        settings.application_library_path.push_back(executablePath.UTF8String);
      }
    }

    // No application bundle specified.  Try a known location from the main bundle's Info.plist.
    if (settings.application_library_path.size() == 0) {
      NSString* libraryName = [mainBundle objectForInfoDictionaryKey:@"FLTLibraryPath"];
      NSString* libraryPath = [mainBundle pathForResource:libraryName ofType:@""];
      if (libraryPath.length > 0) {
        NSString* executablePath = [NSBundle bundleWithPath:libraryPath].executablePath;
        if (executablePath.length > 0) {
          settings.application_library_path.push_back(executablePath.UTF8String);
        }
      }
    }

    // In case the application bundle is still not specified, look for the App.framework in the
    // Frameworks directory.
    if (settings.application_library_path.size() == 0) {
      NSString* applicationFrameworkPath = [mainBundle pathForResource:@"Frameworks/App.framework"
                                                                ofType:@""];
      if (applicationFrameworkPath.length > 0) {
        NSString* executablePath =
            [NSBundle bundleWithPath:applicationFrameworkPath].executablePath;
        if (executablePath.length > 0) {
          settings.application_library_path.push_back(executablePath.UTF8String);
        }
      }
    }
  }

  // Checks to see if the flutter assets directory is already present.
  if (settings.assets_path.size() == 0) {
    NSString* assetsName = [FlutterDartProject flutterAssetsName:bundle];
    NSString* assetsPath = [bundle pathForResource:assetsName ofType:@""];

    if (assetsPath.length == 0) {
      assetsPath = [mainBundle pathForResource:assetsName ofType:@""];
    }
      
    //从外部设置flutter_assets的路径
    if (assetsPath.length == 0) {
        assetsPath = [mainBundle pathForResource:@"flutter_assets" ofType:@"bundle"];
    }

    if (assetsPath.length == 0) {
      NSLog(@"Failed to find assets path for \"%@\"", assetsName);
    } else {
      settings.assets_path = assetsPath.UTF8String;

      // Check if there is an application kernel snapshot in the assets directory we could
      // potentially use.  Looking for the snapshot makes sense only if we have a VM that can use
      // it.
      if (!flutter::DartVM::IsRunningPrecompiledCode()) {
        NSURL* applicationKernelSnapshotURL =
            [NSURL URLWithString:@(kApplicationKernelSnapshotFileName)
                   relativeToURL:[NSURL fileURLWithPath:assetsPath]];
        if ([[NSFileManager defaultManager] fileExistsAtPath:applicationKernelSnapshotURL.path]) {
          settings.application_kernel_asset = applicationKernelSnapshotURL.path.UTF8String;
        } else {
          NSLog(@"Failed to find snapshot: %@", applicationKernelSnapshotURL.path);
        }
      }
    }
  }

  //数据段分离 从外部设置路径
  NSString *isolatePath = [[NSBundle mainBundle] pathForResource:@"_kDartIsolateSnapshotData" ofType:@"dat"];
  if (isolatePath) {
    NSData *data = [[NSData dataWithContentsOfFile:isolatePath] retain];
    if (data) {
      const uint8_t* mapData = (const uint8_t *)[data bytes];
      settings.isolate_snapshot_data_ptr = mapData;
    }
  }

  NSString *vmPath = [[NSBundle mainBundle] pathForResource:@"_kDartVmSnapshotData" ofType:@"dat"];
  if (vmPath) {
    NSData *data = [[NSData dataWithContentsOfFile:vmPath] retain];
    if (data) {
      const uint8_t* mapData = (const uint8_t *)[data bytes];
      settings.vm_snapshot_data_ptr = mapData;
    }
  }

#if FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG
  // There are no ownership concerns here as all mappings are owned by the
  // embedder and not the engine.
  auto make_mapping_callback = [](const uint8_t* mapping, size_t size) {
    return [mapping, size]() { return std::make_unique<fml::NonOwnedMapping>(mapping, size); };
  };

  settings.dart_library_sources_kernel =
      make_mapping_callback(kPlatformStrongDill, kPlatformStrongDillSize);
#endif  // FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG

  return settings;
}

@implementation FlutterDartProject {
  flutter::Settings _settings;
}

#pragma mark - Override base class designated initializers

- (instancetype)init {
  return [self initWithPrecompiledDartBundle:nil];
}

#pragma mark - Designated initializers

- (instancetype)initWithPrecompiledDartBundle:(nullable NSBundle*)bundle {
  self = [super init];

  if (self) {
    _settings = DefaultSettingsForProcess(bundle);
  }

  return self;
}

#pragma mark - Settings accessors

- (const flutter::Settings&)settings {
  return _settings;
}

- (flutter::RunConfiguration)runConfiguration {
  return [self runConfigurationForEntrypoint:nil];
}

- (flutter::RunConfiguration)runConfigurationForEntrypoint:(nullable NSString*)entrypointOrNil {
  return [self runConfigurationForEntrypoint:entrypointOrNil libraryOrNil:nil];
}

- (flutter::RunConfiguration)runConfigurationForEntrypoint:(nullable NSString*)entrypointOrNil
                                              libraryOrNil:(nullable NSString*)dartLibraryOrNil {
  auto config = flutter::RunConfiguration::InferFromSettings(_settings);
  if (dartLibraryOrNil && entrypointOrNil) {
    config.SetEntrypointAndLibrary(std::string([entrypointOrNil UTF8String]),
                                   std::string([dartLibraryOrNil UTF8String]));

  } else if (entrypointOrNil) {
    config.SetEntrypoint(std::string([entrypointOrNil UTF8String]));
  }
  return config;
}

#pragma mark - Assets-related utilities

+ (NSString*)flutterAssetsName:(NSBundle*)bundle {
  if (bundle == nil) {
    bundle = [NSBundle bundleWithIdentifier:[FlutterDartProject defaultBundleIdentifier]];
  }
  if (bundle == nil) {
    bundle = [NSBundle mainBundle];
  }
  NSString* flutterAssetsName = [bundle objectForInfoDictionaryKey:@"FLTAssetsPath"];
  if (flutterAssetsName == nil) {
    flutterAssetsName = @"Frameworks/App.framework/flutter_assets";
  }
  return flutterAssetsName;
}

+ (NSString*)lookupKeyForAsset:(NSString*)asset {
  return [self lookupKeyForAsset:asset fromBundle:nil];
}

+ (NSString*)lookupKeyForAsset:(NSString*)asset fromBundle:(nullable NSBundle*)bundle {
  NSString* flutterAssetsName = [FlutterDartProject flutterAssetsName:bundle];
  return [NSString stringWithFormat:@"%@/%@", flutterAssetsName, asset];
}

+ (NSString*)lookupKeyForAsset:(NSString*)asset fromPackage:(NSString*)package {
  return [self lookupKeyForAsset:asset fromPackage:package fromBundle:nil];
}

+ (NSString*)lookupKeyForAsset:(NSString*)asset
                   fromPackage:(NSString*)package
                    fromBundle:(nullable NSBundle*)bundle {
  return [self lookupKeyForAsset:[NSString stringWithFormat:@"packages/%@/%@", package, asset]
                      fromBundle:bundle];
}

+ (NSString*)defaultBundleIdentifier {
  return @"io.flutter.flutter.app";
}

#pragma mark - Settings utilities

- (void)setPersistentIsolateData:(NSData*)data {
  if (data == nil) {
    return;
  }

  NSData* persistent_isolate_data = [data copy];
  fml::NonOwnedMapping::ReleaseProc data_release_proc = [persistent_isolate_data](auto, auto) {
    [persistent_isolate_data release];
  };
  _settings.persistent_isolate_data = std::make_shared<fml::NonOwnedMapping>(
      static_cast<const uint8_t*>(persistent_isolate_data.bytes),  // bytes
      persistent_isolate_data.length,                              // byte length
      data_release_proc                                            // release proc
  );
}

@end
