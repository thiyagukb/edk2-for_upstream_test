/** @file
  Platform BDS customizations.

  Copyright (c) 2004 - 2019, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Protocol/PlatformBootManagerOverride.h>


STATIC UNIVERSAL_PAYLOAD_PLATFORM_BOOT_MANAGER_OVERRIDE_PROTOCOL mUniversalPayloadPlatformBootManager = {
  PlatformBootManagerBeforeConsole,
  PlatformBootManagerAfterConsole,
  PlatformBootManagerWaitCallback,
  PlatformBootManagerUnableToBoot,
};

// Entry point of this driver
//
EFI_STATUS
EFIAPI
InitPlatformBootManagerLib (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
    EFI_STATUS                   Status;
    Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gUniversalPayloadPlatformBootManagerOverrideProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mUniversalPayloadPlatformBootManager
                  );
    return Status;
}