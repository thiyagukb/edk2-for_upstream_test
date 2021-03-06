/** @file
  The header file of bootloader support DXE.

Copyright (c) 2021, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/
#ifndef __DXE_BOOTLOADER_SUPPORT_H__
#define __DXE_BOOTLOADER_SUPPORT_H__

#include <PiDxe.h>

#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>
#include <Library/HobLib.h>

#include <Guid/Acpi.h>
#include <Guid/SmBios.h>
#include <UniversalPayload/AcpiTable.h>
#include <Guid/GraphicsInfoHob.h>
#include <Guid/AcpiBoardInfoGuid.h>

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/MemoryMappedConfigurationSpaceAccessTable.h>

#endif
