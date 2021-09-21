/** @file
  PCI Segment Information Library that returns one segment whose
  segment base address is retrieved from AcpiBoardInfo HOB.

  Copyright (c) 2020, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Guid/AcpiBoardInfoGuid.h>

#include <Library/HobLib.h>
#include <Library/PciSegmentInfoLib.h>
#include <Library/DebugLib.h>
#include <UniversalPayload/AcpiTable.h>
#include <Library/AcpiParserLib.h>

STATIC PCI_SEGMENT_INFO mPciSegment0 = {
  0,  // Segment number
  0,  // To be fixed later
  0,  // Start bus number
  255 // End bus number
};

/**
  Return an array of PCI_SEGMENT_INFO holding the segment information.

  Note: The returned array/buffer is owned by callee.

  @param  Count  Return the count of segments.

  @retval A callee owned array holding the segment information.
**/
PCI_SEGMENT_INFO *
EFIAPI
GetPciSegmentInfo (
  UINTN  *Count
  )
{
  EFI_HOB_GUID_TYPE  *GuidHob;
  ACPI_BOARD_INFO    AcpiBoardInfo;
  UNIVERSAL_PAYLOAD_ACPI_TABLE  *AcpiTableHob;

  ASSERT (Count != NULL);
  if (Count == NULL) {
    return NULL;
  }

  if (mPciSegment0.BaseAddress == 0) {
    //
    // Find the acpi table
    //
    GuidHob = GetFirstGuidHob (&gUniversalPayloadAcpiTableGuid);
    ASSERT (GuidHob != NULL);

    AcpiTableHob = (UNIVERSAL_PAYLOAD_ACPI_TABLE *) GET_GUID_HOB_DATA (GuidHob);
    
    ParseAcpiInfo(AcpiTableHob->Rsdp,&AcpiBoardInfo);

    mPciSegment0.BaseAddress = AcpiBoardInfo.PcieBaseAddress;
  }
  *Count = 1;
  return &mPciSegment0;
}
