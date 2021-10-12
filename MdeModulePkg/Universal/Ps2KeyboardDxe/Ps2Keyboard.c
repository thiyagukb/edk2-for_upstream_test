/** @file

  PS/2 Keyboard driver. Routines that interacts with callers,
  conforming to EFI driver model

Copyright (c) 2006 - 2016, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Ps2Keyboard.h"

typedef struct {
  ACPI_HID_DEVICE_PATH      Ps2keyboard;
  EFI_DEVICE_PATH_PROTOCOL  End;
} PS2_KEYBOARD_DEVICE_PATH;


PS2_KEYBOARD_DEVICE_PATH gPnpPs2KeyboardDevicePath = {

  {
    {
      ACPI_DEVICE_PATH,
      ACPI_DP,
      {
        (UINT8) (sizeof(ACPI_HID_DEVICE_PATH)),
        (UINT8) ((sizeof(ACPI_HID_DEVICE_PATH)) >> 8)
      }
    },
    EISA_PNP_ID(0x0303), // HID
    0                    // UID
  },

 { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

/**
  Free the waiting key notify list.

  @param ListHead  Pointer to list head

  @retval EFI_INVALID_PARAMETER  ListHead is NULL
  @retval EFI_SUCCESS            Success to free NotifyList
**/
EFI_STATUS
KbdFreeNotifyList (
  IN OUT LIST_ENTRY           *ListHead
  )
{
  KEYBOARD_CONSOLE_IN_EX_NOTIFY *NotifyNode;

  if (ListHead == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  while (!IsListEmpty (ListHead)) {
    NotifyNode = CR (
                   ListHead->ForwardLink,
                   KEYBOARD_CONSOLE_IN_EX_NOTIFY,
                   NotifyEntry,
                   KEYBOARD_CONSOLE_IN_EX_NOTIFY_SIGNATURE
                   );
    RemoveEntryList (ListHead->ForwardLink);
    gBS->FreePool (NotifyNode);
  }

  return EFI_SUCCESS;
}

/**
  The module Entry Point for module Ps2Keyboard.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

**/
EFI_STATUS
EFIAPI
InitializePs2Keyboard(
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{

  EFI_STATUS                  Status;
  EFI_STATUS                  Status1;
  KEYBOARD_CONSOLE_IN_DEV     *ConsoleIn;
  UINT8                       Data;
  EFI_STATUS_CODE_VALUE       StatusCode;
  EFI_DEVICE_PATH_PROTOCOL    *DevicePath;

  StatusCode = 0;
  DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)&gPnpPs2KeyboardDevicePath;

  //
  // Report that the keyboard is being enabled
  //
  REPORT_STATUS_CODE (
    EFI_PROGRESS_CODE,
    EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_ENABLE
    );

  //
  // Allocate private data
  //
  ConsoleIn = AllocateZeroPool (sizeof (KEYBOARD_CONSOLE_IN_DEV));
  if (ConsoleIn == NULL) {

    Status      = EFI_OUT_OF_RESOURCES;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  //
  // Setup the device instance
  //
  ConsoleIn->Signature              = KEYBOARD_CONSOLE_IN_DEV_SIGNATURE;
  ConsoleIn->Handle                 = ImageHandle;
  (ConsoleIn->ConIn).Reset          = KeyboardEfiReset;
  (ConsoleIn->ConIn).ReadKeyStroke  = KeyboardReadKeyStroke;
  ConsoleIn->DataRegisterAddress    = KEYBOARD_8042_DATA_REGISTER;
  ConsoleIn->StatusRegisterAddress  = KEYBOARD_8042_STATUS_REGISTER;
  ConsoleIn->CommandRegisterAddress = KEYBOARD_8042_COMMAND_REGISTER;
  ConsoleIn->DevicePath             = DevicePath;

  ConsoleIn->ConInEx.Reset               = KeyboardEfiResetEx;
  ConsoleIn->ConInEx.ReadKeyStrokeEx     = KeyboardReadKeyStrokeEx;
  ConsoleIn->ConInEx.SetState            = KeyboardSetState;
  ConsoleIn->ConInEx.RegisterKeyNotify   = KeyboardRegisterKeyNotify;
  ConsoleIn->ConInEx.UnregisterKeyNotify = KeyboardUnregisterKeyNotify;

  InitializeListHead (&ConsoleIn->NotifyList);

  //
  // Fix for random hangs in System waiting for the Key if no KBC is present in BIOS.
  // When KBC decode (IO port 0x60/0x64 decode) is not enabled,
  // KeyboardRead will read back as 0xFF and return status is EFI_SUCCESS.
  // So instead we read status register to detect after read if KBC decode is enabled.
  //

  //
  // Return code is ignored on purpose.
  //
  if (!PcdGetBool (PcdFastPS2Detection)) {

    KeyboardRead (ConsoleIn, &Data);
    if ((KeyReadStatusRegister (ConsoleIn) & (KBC_PARE | KBC_TIM)) == (KBC_PARE | KBC_TIM)) {
      //
      // If nobody decodes KBC I/O port, it will read back as 0xFF.
      // Check the Time-Out and Parity bit to see if it has an active KBC in system
      //

      Status      = EFI_DEVICE_ERROR;
      StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_NOT_DETECTED;
      goto ErrorExit;
    }
  }

  //
  // Setup the WaitForKey event
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  KeyboardWaitForKey,
                  ConsoleIn,
                  &((ConsoleIn->ConIn).WaitForKey)
                  );

  if (EFI_ERROR (Status)) {

    Status      = EFI_OUT_OF_RESOURCES;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  //
  // Setup the WaitForKeyEx event
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  KeyboardWaitForKeyEx,
                  ConsoleIn,
                  &(ConsoleIn->ConInEx.WaitForKeyEx)
                  );

  if (EFI_ERROR (Status)) {

    Status      = EFI_OUT_OF_RESOURCES;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  //
  // Setup a periodic timer, used for reading keystrokes at a fixed interval
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  KeyboardTimerHandler,
                  ConsoleIn,
                  &ConsoleIn->TimerEvent
                  );

  if (EFI_ERROR (Status)) {

    Status      = EFI_OUT_OF_RESOURCES;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  Status = gBS->SetTimer (
                  ConsoleIn->TimerEvent,
                  TimerPeriodic,
                  KEYBOARD_TIMER_INTERVAL
                  );

  if (EFI_ERROR (Status)) {

    Status      = EFI_OUT_OF_RESOURCES;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  KeyNotifyProcessHandler,
                  ConsoleIn,
                  &ConsoleIn->KeyNotifyProcessEvent
                  );

  if (EFI_ERROR (Status)) {

    Status      = EFI_OUT_OF_RESOURCES;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  REPORT_STATUS_CODE (
    EFI_PROGRESS_CODE,
    EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_PRESENCE_DETECT
    );

  //
  // Reset the keyboard device
  //
  Status = ConsoleIn->ConInEx.Reset (&ConsoleIn->ConInEx, FeaturePcdGet (PcdPs2KbdExtendedVerification));
  if (EFI_ERROR (Status)) {

    Status      = EFI_DEVICE_ERROR;
    StatusCode  = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_NOT_DETECTED;
    goto ErrorExit;
  }

  REPORT_STATUS_CODE (
    EFI_PROGRESS_CODE,
    EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_DETECTED
    );

  ConsoleIn->ControllerNameTable = NULL;


  //
  // Install protocol interfaces for the keyboard device.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiDevicePathProtocolGuid,
                  DevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &ConsoleIn->ConIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &ConsoleIn->ConInEx,
                  NULL
                  );

  if (EFI_ERROR (Status)) {

    ASSERT_RETURN_ERROR(Status);
    StatusCode = EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR;
    goto ErrorExit;
  }

  return Status;

ErrorExit:
  //
  // Report error code
  //
 if (StatusCode != 0) {
    REPORT_STATUS_CODE (
      EFI_ERROR_CODE | EFI_ERROR_MINOR,
      StatusCode
      );
  }

  if ((ConsoleIn != NULL) && (ConsoleIn->ConIn.WaitForKey != NULL)) {
    gBS->CloseEvent (ConsoleIn->ConIn.WaitForKey);
  }

  if ((ConsoleIn != NULL) && (ConsoleIn->TimerEvent != NULL)) {
    gBS->CloseEvent (ConsoleIn->TimerEvent);
  }
  if ((ConsoleIn != NULL) && (ConsoleIn->ConInEx.WaitForKeyEx != NULL)) {
    gBS->CloseEvent (ConsoleIn->ConInEx.WaitForKeyEx);
  }
  if ((ConsoleIn != NULL) && (ConsoleIn->KeyNotifyProcessEvent != NULL)) {
    gBS->CloseEvent (ConsoleIn->KeyNotifyProcessEvent);
  }
  KbdFreeNotifyList (&ConsoleIn->NotifyList);
  if ((ConsoleIn != NULL) && (ConsoleIn->ControllerNameTable != NULL)) {
    FreeUnicodeStringTable (ConsoleIn->ControllerNameTable);
  }
  //
  // Since there will be no timer handler for keyboard input any more,
  // exhaust input data just in case there is still keyboard data left
  //
  if (ConsoleIn != NULL) {
    Status1 = EFI_SUCCESS;
    while (!EFI_ERROR (Status1) && (Status != EFI_DEVICE_ERROR)) {
      Status1 = KeyboardRead (ConsoleIn, &Data);;
    }
  }

  if (ConsoleIn != NULL) {
    gBS->FreePool (ConsoleIn);
  }

  return Status;
}

