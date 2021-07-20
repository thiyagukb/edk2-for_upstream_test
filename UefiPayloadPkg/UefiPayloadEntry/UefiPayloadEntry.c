/** @file

  Copyright (c) 2014 - 2021, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UefiPayloadEntry.h"

STATIC UINT32 mTopOfLowerUsableDram = 0;



/**
   Callback function to find TOLUD (Top of Lower Usable DRAM)

   Estimate where TOLUD (Top of Lower Usable DRAM) resides. The exact position
   would require platform specific code.

   @param MemoryMapEntry         Memory map entry info got from bootloader.
   @param Params                 Not used for now.

  @retval EFI_SUCCESS            Successfully updated mTopOfLowerUsableDram.
**/
EFI_STATUS
FindToludCallback (
  IN MEMROY_MAP_ENTRY          *MemoryMapEntry,
  IN VOID                      *Params
  )
{
  //
  // This code assumes that the memory map on this x86 machine below 4GiB is continous
  // until TOLUD. In addition it assumes that the bootloader provided memory tables have
  // no "holes" and thus the first memory range not covered by e820 marks the end of
  // usable DRAM. In addition it's assumed that every reserved memory region touching
  // usable RAM is also covering DRAM, everything else that is marked reserved thus must be
  // MMIO not detectable by bootloader/OS
  //

  //
  // Skip memory types not RAM or reserved
  //
  if ((MemoryMapEntry->Type == E820_UNUSABLE) || (MemoryMapEntry->Type == E820_DISABLED) ||
    (MemoryMapEntry->Type == E820_PMEM)) {
    return EFI_SUCCESS;
  }

  //
  // Skip resources above 4GiB
  //
  if ((MemoryMapEntry->Base + MemoryMapEntry->Size) > 0x100000000ULL) {
    return EFI_SUCCESS;
  }

  if ((MemoryMapEntry->Type == E820_RAM) || (MemoryMapEntry->Type == E820_ACPI) ||
    (MemoryMapEntry->Type == E820_NVS)) {
    //
    // It's usable DRAM. Update TOLUD.
    //
    if (mTopOfLowerUsableDram < (MemoryMapEntry->Base + MemoryMapEntry->Size)) {
      mTopOfLowerUsableDram = (UINT32)(MemoryMapEntry->Base + MemoryMapEntry->Size);
    }
  } else {
    //
    // It might be 'reserved DRAM' or 'MMIO'.
    //
    // If it touches usable DRAM at Base assume it's DRAM as well,
    // as it could be bootloader installed tables, TSEG, GTT, ...
    //
    if (mTopOfLowerUsableDram == MemoryMapEntry->Base) {
      mTopOfLowerUsableDram = (UINT32)(MemoryMapEntry->Base + MemoryMapEntry->Size);
    }
  }

  return EFI_SUCCESS;
}


/**
   Callback function to build resource descriptor HOB

   This function build a HOB based on the memory map entry info.
   Only add EFI_RESOURCE_SYSTEM_MEMORY.

   @param MemoryMapEntry         Memory map entry info got from bootloader.
   @param Params                 Not used for now.

  @retval RETURN_SUCCESS        Successfully build a HOB.
**/
EFI_STATUS
MemInfoCallback (
  IN MEMROY_MAP_ENTRY          *MemoryMapEntry,
  IN VOID                      *Params
  )
{
  EFI_PHYSICAL_ADDRESS         Base;
  EFI_RESOURCE_TYPE            Type;
  UINT64                       Size;
  EFI_RESOURCE_ATTRIBUTE_TYPE  Attribue;

  //
  // Skip everything not known to be usable DRAM.
  // It will be added later.
  //
  if ((MemoryMapEntry->Type != E820_RAM) && (MemoryMapEntry->Type != E820_ACPI) &&
    (MemoryMapEntry->Type != E820_NVS)) {
    return RETURN_SUCCESS;
  }

  Type    = EFI_RESOURCE_SYSTEM_MEMORY;
  Base    = MemoryMapEntry->Base;
  Size    = MemoryMapEntry->Size;

  Attribue = EFI_RESOURCE_ATTRIBUTE_PRESENT |
             EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
             EFI_RESOURCE_ATTRIBUTE_TESTED |
             EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
             EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
             EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
             EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE;

  BuildResourceDescriptorHob (Type, Attribue, (EFI_PHYSICAL_ADDRESS)Base, Size);
  DEBUG ((DEBUG_INFO , "buildhob: base = 0x%lx, size = 0x%lx, type = 0x%x\n", Base, Size, Type));

  if (MemoryMapEntry->Type == E820_ACPI) {
    BuildMemoryAllocationHob (Base, Size, EfiACPIReclaimMemory);
  } else if (MemoryMapEntry->Type == E820_NVS) {
    BuildMemoryAllocationHob (Base, Size, EfiACPIMemoryNVS);
  }

  return RETURN_SUCCESS;
}


/**
  It will build HOBs based on information from bootloaders.

  @retval EFI_SUCCESS        If it completed successfully.
  @retval Others             If it failed to build required HOBs.
**/
EFI_STATUS
BuildHobFromBl (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_PEI_GRAPHICS_INFO_HOB        GfxInfo;
  EFI_PEI_GRAPHICS_INFO_HOB        *NewGfxInfo;
  EFI_PEI_GRAPHICS_DEVICE_INFO_HOB GfxDeviceInfo;
  EFI_PEI_GRAPHICS_DEVICE_INFO_HOB *NewGfxDeviceInfo;
  UNIVERSAL_PAYLOAD_SMBIOS_TABLE    SmbiosTable;
  UNIVERSAL_PAYLOAD_SMBIOS_TABLE    *NewSmbiosTable;
  UNIVERSAL_PAYLOAD_ACPI_TABLE     *AcpiTableHob;
  UNIVERSAL_PAYLOAD_ACPI_TABLE     *NewAcpiTableHob;

  EFI_SMRAM_HOB_DESCRIPTOR_BLOCK   *SmramHob;
  UINT32                           Size;
  UINT8                            Buffer[200];
  PLD_SMM_REGISTERS                *SmmRegisterHob;
  VOID                             *NewHob;
  UINT32                           Index;
  PLD_S3_COMMUNICATION             *PldS3Info;
  SPI_FLASH_INFO                   SpiFlashInfo;
  SPI_FLASH_INFO                   *NewSpiFlashInfo;
  NV_VARIABLE_INFO                 NvVariableInfo;
  NV_VARIABLE_INFO                 *NewNvVariableInfo;
  //
  // First find TOLUD
  //
  DEBUG ((DEBUG_INFO , "Guessing Top of Lower Usable DRAM:\n"));
  Status = ParseMemoryInfo (FindToludCallback, NULL);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  DEBUG ((DEBUG_INFO , "Assuming TOLUD = 0x%x\n", mTopOfLowerUsableDram));

  //
  // Parse memory info and build memory HOBs for Usable RAM
  //
  DEBUG ((DEBUG_INFO , "Building ResourceDescriptorHobs for usable memory:\n"));
  Status = ParseMemoryInfo (MemInfoCallback, NULL);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  //
  // Create guid hob for frame buffer information
  //
  Status = ParseGfxInfo (&GfxInfo);
  if (!EFI_ERROR (Status)) {
    NewGfxInfo = BuildGuidHob (&gEfiGraphicsInfoHobGuid, sizeof (GfxInfo));
    ASSERT (NewGfxInfo != NULL);
    CopyMem (NewGfxInfo, &GfxInfo, sizeof (GfxInfo));
    DEBUG ((DEBUG_INFO, "Created graphics info hob\n"));
  }


  Status = ParseGfxDeviceInfo (&GfxDeviceInfo);
  if (!EFI_ERROR (Status)) {
    NewGfxDeviceInfo = BuildGuidHob (&gEfiGraphicsDeviceInfoHobGuid, sizeof (GfxDeviceInfo));
    ASSERT (NewGfxDeviceInfo != NULL);
    CopyMem (NewGfxDeviceInfo, &GfxDeviceInfo, sizeof (GfxDeviceInfo));
    DEBUG ((DEBUG_INFO, "Created graphics device info hob\n"));
  }


  //
  // Create smbios table guid hob
  //
  Status = ParseSmbiosTable(&SmbiosTable);
  if (!EFI_ERROR (Status)) {
    NewSmbiosTable = BuildGuidHob (&gEfiSmbiosTableGuid, sizeof (SmbiosTable));
    ASSERT (NewSmbiosTable != NULL);
    CopyMem (NewSmbiosTable, &SmbiosTable, sizeof (SmbiosTable));
    DEBUG ((DEBUG_INFO, "Detected Smbios Table at 0x%lx\n", SmbiosTable.TableAddress));
  }

  //
  // Create acpi table guid hob
  //
  Status = ParseAcpiTableInfo(&AcpiTableHob);
  ASSERT_EFI_ERROR (Status);
  if (!EFI_ERROR (Status)) {
    NewAcpiTableHob = BuildGuidHob (&gEfiAcpiTableGuid, sizeof (AcpiTableHob));
    ASSERT (NewAcpiTableHob != NULL);
    CopyMem (NewAcpiTableHob, &AcpiTableHob, sizeof (AcpiTableHob));
    DEBUG ((DEBUG_INFO, "Detected ACPI Table at 0x%lx\n", AcpiTableHob.TableAddress));
  }

  //
  // Create SMRAM information HOB
  //
  SmramHob = (EFI_SMRAM_HOB_DESCRIPTOR_BLOCK *)Buffer;
  Size     = sizeof (Buffer);
  Status = GetSmramInfo (SmramHob, &Size);
  DEBUG((DEBUG_INFO, "GetSmramInfo = %r, data Size = 0x%x\n", Status, Size));
  if (!EFI_ERROR (Status)) {
    NewHob = BuildGuidHob (&gEfiSmmSmramMemoryGuid, Size);
    ASSERT (NewHob != NULL);
    CopyMem (NewHob, SmramHob, Size);
    DEBUG((DEBUG_INFO, "Region count = 0x%x\n", SmramHob->NumberOfSmmReservedRegions));
    for (Index = 0; Index < SmramHob->NumberOfSmmReservedRegions; Index++ ) {
      DEBUG((DEBUG_INFO, "CpuStart[%d] = 0x%lx\n", Index, SmramHob->Descriptor[Index].CpuStart));
      DEBUG((DEBUG_INFO, "base[%d]     = 0x%lx\n", Index, SmramHob->Descriptor[Index].PhysicalStart));
      DEBUG((DEBUG_INFO, "size[%d]     = 0x%lx\n", Index, SmramHob->Descriptor[Index].PhysicalSize));
      DEBUG((DEBUG_INFO, "State[%d]    = 0x%lx\n", Index, SmramHob->Descriptor[Index].RegionState));
    }
  }

  //
  // Create SMM register information HOB
  //
  SmmRegisterHob = (PLD_SMM_REGISTERS *)Buffer;
  Size           = sizeof (Buffer);
  Status = GetSmmRegisterInfo (SmmRegisterHob, &Size);
  DEBUG((DEBUG_INFO, "GetSmmRegisterInfo = %r, data Size = 0x%x\n", Status, Size));
  if (!EFI_ERROR (Status)) {
    NewHob = BuildGuidHob (&gPldSmmRegisterInfoGuid, Size);
    ASSERT (NewHob != NULL);
    CopyMem (NewHob, SmramHob, Size);
    DEBUG((DEBUG_INFO, "SMM register count = 0x%x\n", SmmRegisterHob->Count));
    for (Index = 0; Index < SmmRegisterHob->Count; Index++ ) {
      DEBUG((DEBUG_INFO, "ID[%d]            = 0x%lx\n", Index, SmmRegisterHob->Registers[Index].Id));
      DEBUG((DEBUG_INFO, "Value[%d]         = 0x%lx\n", Index, SmmRegisterHob->Registers[Index].Value));
      DEBUG((DEBUG_INFO, "AddressSpaceId    = 0x%x\n",  SmmRegisterHob->Registers[Index].Address.AddressSpaceId));
      DEBUG((DEBUG_INFO, "RegisterBitWidth  = 0x%x\n",  SmmRegisterHob->Registers[Index].Address.RegisterBitWidth));
      DEBUG((DEBUG_INFO, "RegisterBitOffset = 0x%x\n",  SmmRegisterHob->Registers[Index].Address.RegisterBitOffset));
      DEBUG((DEBUG_INFO, "AccessSize        = 0x%x\n",  SmmRegisterHob->Registers[Index].Address.AccessSize));
      DEBUG((DEBUG_INFO, "Address           = 0x%lx\n", SmmRegisterHob->Registers[Index].Address.Address));
    }
  }

  //
  // Create SMM S3 information HOB
  //
  PldS3Info = (PLD_S3_COMMUNICATION *)Buffer;
  Size      = sizeof (Buffer);
  Status = GetPldS3CommunicationInfo (PldS3Info, &Size);
  DEBUG((DEBUG_INFO, "GetPldS3Info = %r, data Size = 0x%x\n", Status, Size));
  if (!EFI_ERROR (Status)) {
    NewHob = BuildGuidHob (&gPldS3CommunicationGuid, Size);
    ASSERT (NewHob != NULL);
    CopyMem (NewHob, SmramHob, Size);
    PldS3Info = (PLD_S3_COMMUNICATION *)NewHob;
  }

  //
  // Create a HOB for SPI flash info
  //
  Status = ParseSpiFlashInfo(&SpiFlashInfo);
  if (!EFI_ERROR (Status)) {
    NewSpiFlashInfo = BuildGuidHob (&gSpiFlashInfoGuid, sizeof (SPI_FLASH_INFO));
    ASSERT (NewSpiFlashInfo != NULL);
    CopyMem (NewSpiFlashInfo, &SpiFlashInfo, sizeof (SPI_FLASH_INFO));
    DEBUG ((DEBUG_INFO, "Flags=0x%x, SPI PCI Base=0x%lx\n", SpiFlashInfo.Flags, SpiFlashInfo.SpiAddress.Address));
  }

  //
  // Create a hob for NV variable
  //
  Status = ParseNvVariableInfo(&NvVariableInfo);
  if (!EFI_ERROR (Status)) {
    NewNvVariableInfo = BuildGuidHob (&gNvVariableInfoGuid, sizeof (NV_VARIABLE_INFO));
    ASSERT (NewNvVariableInfo != NULL);
    CopyMem (NewNvVariableInfo, &NvVariableInfo, sizeof (NV_VARIABLE_INFO));
    DEBUG ((DEBUG_INFO, "VarStoreBase=0x%x, length=0x%x\n", NvVariableInfo.VariableStoreBase, NvVariableInfo.VariableStoreSize));
  }

  //
  // Parse platform specific information.
  //
  Status = ParsePlatformInfo ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Error when parsing platform info, Status = %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}


/**
  This function will build some generic HOBs that doesn't depend on information from bootloaders.

**/
VOID
BuildGenericHob (
  VOID
  )
{
  UINT32                           RegEax;
  UINT8                            PhysicalAddressBits;
  EFI_RESOURCE_ATTRIBUTE_TYPE      ResourceAttribute;

  // The UEFI payload FV
  BuildMemoryAllocationHob (PcdGet32 (PcdPayloadFdMemBase), PcdGet32 (PcdPayloadFdMemSize), EfiBootServicesData);

  //
  // Build CPU memory space and IO space hob
  //
  AsmCpuid (0x80000000, &RegEax, NULL, NULL, NULL);
  if (RegEax >= 0x80000008) {
    AsmCpuid (0x80000008, &RegEax, NULL, NULL, NULL);
    PhysicalAddressBits = (UINT8) RegEax;
  } else {
    PhysicalAddressBits  = 36;
  }

  BuildCpuHob (PhysicalAddressBits, 16);

  //
  // Report Local APIC range, cause sbl HOB to be NULL, comment now
  //
  ResourceAttribute = (
      EFI_RESOURCE_ATTRIBUTE_PRESENT |
      EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
      EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_TESTED
  );
  BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_MAPPED_IO, ResourceAttribute, 0xFEC80000, SIZE_512KB);
  BuildMemoryAllocationHob ( 0xFEC80000, SIZE_512KB, EfiMemoryMappedIO);

}


/**
  Entry point to the C language phase of UEFI payload.

  @retval      It will not return if SUCCESS, and return error when passing bootloader parameter.
**/
EFI_STATUS
EFIAPI
PayloadEntry (
  IN UINTN                     BootloaderParameter
  )
{
  EFI_STATUS                    Status;
  PHYSICAL_ADDRESS              DxeCoreEntryPoint;
  UINTN                         MemBase;
  UINTN                         HobMemBase;
  UINTN                         HobMemTop;
  EFI_PEI_HOB_POINTERS          Hob;

  // Call constructor for all libraries
  ProcessLibraryConstructorList ();

  DEBUG ((DEBUG_INFO, "GET_BOOTLOADER_PARAMETER() = 0x%lx\n", GET_BOOTLOADER_PARAMETER()));
  DEBUG ((DEBUG_INFO, "sizeof(UINTN) = 0x%x\n", sizeof(UINTN)));

  // Initialize floating point operating environment to be compliant with UEFI spec.
  InitializeFloatingPointUnits ();

  // HOB region is used for HOB and memory allocation for this module
  MemBase    = PcdGet32 (PcdPayloadFdMemBase);
  HobMemBase = ALIGN_VALUE (MemBase + PcdGet32 (PcdPayloadFdMemSize), SIZE_1MB);
  HobMemTop  = HobMemBase + FixedPcdGet32 (PcdSystemMemoryUefiRegionSize);

  HobConstructor ((VOID *)MemBase, (VOID *)HobMemTop, (VOID *)HobMemBase, (VOID *)HobMemTop);

  // Build HOB based on information from Bootloader
  Status = BuildHobFromBl ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BuildHobFromBl Status = %r\n", Status));
    return Status;
  }

  // Build other HOBs required by DXE
  BuildGenericHob ();

  // Load the DXE Core
  Status = LoadDxeCore (&DxeCoreEntryPoint);
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "DxeCoreEntryPoint = 0x%lx\n", DxeCoreEntryPoint));

  //
  // Mask off all legacy 8259 interrupt sources
  //
  IoWrite8 (LEGACY_8259_MASK_REGISTER_MASTER, 0xFF);
  IoWrite8 (LEGACY_8259_MASK_REGISTER_SLAVE,  0xFF);

  Hob.HandoffInformationTable = (EFI_HOB_HANDOFF_INFO_TABLE *) GetFirstHob(EFI_HOB_TYPE_HANDOFF);
  HandOffToDxeCore (DxeCoreEntryPoint, Hob);

  // Should not get here
  CpuDeadLoop ();
  return EFI_SUCCESS;
}
