/** @file
  PS/2 Mouse driver. Routines that interacts with callers,
  conforming to EFI driver model.

Copyright (c) 2006 - 2016, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Ps2Mouse.h"
#include "CommPs2.h"

/**
  Start this driver on ControllerHandle by opening a Sio protocol, creating
  PS2_MOUSE_DEV device and install gEfiSimplePointerProtocolGuid finally.

  @param  This                 Protocol instance pointer.
  @param  ControllerHandle     Handle of device to bind driver to
  @param  RemainingDevicePath  Optional parameter use to pick a specific child
                               device to start.

  @retval EFI_SUCCESS          This driver is added to ControllerHandle
  @retval EFI_ALREADY_STARTED  This driver is already running on ControllerHandle
  @retval other                This driver does not support this device

**/
EFI_STATUS
EFIAPI
PS2MouseDriverEntry (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          EmptyStatus;
  PS2_MOUSE_DEV                       *MouseDev;
  UINT8                               Data;
  EFI_TPL                             OldTpl;
  EFI_STATUS_CODE_VALUE               StatusCode;

  StatusCode  = 0;

  //
  // Raise TPL to avoid keyboard operation impact
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  //
  // Allocate private data
  //
  MouseDev = AllocateZeroPool (sizeof (PS2_MOUSE_DEV));
  if (MouseDev == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }
  //
  // Setup the device instance
  //
  MouseDev->Signature       = PS2_MOUSE_DEV_SIGNATURE;
  MouseDev->Handle          = NULL;
  MouseDev->SampleRate      = SampleRate20;
  MouseDev->Resolution      = MouseResolution4;
  MouseDev->Scaling         = Scaling1;
  MouseDev->DataPackageSize = 3;
  MouseDev->DevicePath      = NULL;

  //
  // Resolution = 4 counts/mm
  //
  MouseDev->Mode.ResolutionX                = 4;
  MouseDev->Mode.ResolutionY                = 4;
  MouseDev->Mode.LeftButton                 = TRUE;
  MouseDev->Mode.RightButton                = TRUE;

  MouseDev->SimplePointerProtocol.Reset     = MouseReset;
  MouseDev->SimplePointerProtocol.GetState  = MouseGetState;
  MouseDev->SimplePointerProtocol.Mode      = &(MouseDev->Mode);

  Data = IoRead8 (KBC_CMD_STS_PORT);
  //
  // Fix for random hangs in System waiting for the Key if no KBC is present in BIOS.
  //
  if ((Data & (KBC_PARE | KBC_TIM)) == (KBC_PARE | KBC_TIM)) {
    //
    // If nobody decodes KBC I/O port, it will read back as 0xFF.
    // Check the Time-Out and Parity bit to see if it has an active KBC in system
    //
    Status     = EFI_DEVICE_ERROR;
    StatusCode = EFI_PERIPHERAL_MOUSE | EFI_P_EC_NOT_DETECTED;
    goto ErrorExit;
  }

  if ((Data & KBC_SYSF) != KBC_SYSF) {
    Status = KbcSelfTest ();
    if (EFI_ERROR (Status)) {
      StatusCode = EFI_PERIPHERAL_MOUSE | EFI_P_EC_CONTROLLER_ERROR;
      goto ErrorExit;
    }
  }

  KbcEnableAux ();

  //
  // Reset the mouse
  //
  Status = MouseDev->SimplePointerProtocol.Reset (
                     &MouseDev->SimplePointerProtocol,
                     FeaturePcdGet (PcdPs2MouseExtendedVerification)
                     );
  if (EFI_ERROR (Status)) {
    //
    // mouse not connected
    //
    Status      = EFI_SUCCESS;
    StatusCode  = EFI_PERIPHERAL_MOUSE | EFI_P_EC_NOT_DETECTED;
    goto ErrorExit;
  }


  //
  // Setup the WaitForKey event
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  MouseWaitForInput,
                  MouseDev,
                  &((MouseDev->SimplePointerProtocol).WaitForInput)
                  );
  if (EFI_ERROR (Status)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }
  //
  // Setup a periodic timer, used to poll mouse state
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  PollMouse,
                  MouseDev,
                  &MouseDev->TimerEvent
                  );
  if (EFI_ERROR (Status)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }
  //
  // Start timer to poll mouse (100 samples per second)
  //
  Status = gBS->SetTimer (MouseDev->TimerEvent, TimerPeriodic, 100000);
  if (EFI_ERROR (Status)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  MouseDev->ControllerNameTable = NULL;



  //
  // Install protocol interfaces for the mouse device.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &MouseDev->Handle,
                  &gEfiSimplePointerProtocolGuid,
                  &MouseDev->SimplePointerProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  gBS->RestoreTPL (OldTpl);

  return Status;

ErrorExit:

  if (Status != EFI_DEVICE_ERROR) {
    KbcDisableAux ();
  }


  if ((MouseDev != NULL) && (MouseDev->SimplePointerProtocol.WaitForInput != NULL)) {
    gBS->CloseEvent (MouseDev->SimplePointerProtocol.WaitForInput);
  }

  if ((MouseDev != NULL) && (MouseDev->TimerEvent != NULL)) {
    gBS->CloseEvent (MouseDev->TimerEvent);
  }

  if ((MouseDev != NULL) && (MouseDev->ControllerNameTable != NULL)) {
    FreeUnicodeStringTable (MouseDev->ControllerNameTable);
  }
  
  EmptyStatus = EFI_SUCCESS;
  if (Status != EFI_DEVICE_ERROR) {
    //
    // Since there will be no timer handler for mouse input any more,
    // exhaust input data just in case there is still mouse data left
    //
  
    while (!EFI_ERROR (EmptyStatus)) {
      EmptyStatus = In8042Data (&Data);
    }
  }

  if (MouseDev != NULL) {
    FreePool (MouseDev);
  }

  gBS->RestoreTPL (OldTpl);

  return Status;
}

/**
  Reset the Mouse and do BAT test for it, if ExtendedVerification is TRUE and
  there is a mouse device connected to system.

  @param This                 - Pointer of simple pointer Protocol.
  @param ExtendedVerification - Whether configure mouse parameters. True: do; FALSE: skip.


  @retval EFI_SUCCESS         - The command byte is written successfully.
  @retval EFI_DEVICE_ERROR    - Errors occurred during resetting keyboard.

**/
EFI_STATUS
EFIAPI
MouseReset (
  IN EFI_SIMPLE_POINTER_PROTOCOL    *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  EFI_STATUS    Status;
  PS2_MOUSE_DEV *MouseDev;
  EFI_TPL       OldTpl;
  BOOLEAN       KeyboardEnable;
  UINT8         Data;

  MouseDev = PS2_MOUSE_DEV_FROM_THIS (This);



  KeyboardEnable = FALSE;

  //
  // Raise TPL to avoid keyboard operation impact
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  ZeroMem (&MouseDev->State, sizeof (EFI_SIMPLE_POINTER_STATE));
  MouseDev->StateChanged = FALSE;

  //
  // Exhaust input data
  //
  Status = EFI_SUCCESS;
  while (!EFI_ERROR (Status)) {
    Status = In8042Data (&Data);
  }

  CheckKbStatus (&KeyboardEnable);

  KbcDisableKb ();

  //
  // if there's data block on KBC data port, read it out
  //
  if ((IoRead8 (KBC_CMD_STS_PORT) & KBC_OUTB) == KBC_OUTB) {
    IoRead8 (KBC_DATA_PORT);
  }

  Status = EFI_SUCCESS;
  //
  // The PS2 mouse driver reset behavior is always successfully return no matter whether or not there is mouse connected to system.
  // This behavior is needed by performance speed. The following mouse command only successfully finish when mouse device is
  // connected to system, so if PS2 mouse device not connect to system or user not ask for, we skip the mouse configuration and enabling
  //
  if (ExtendedVerification && CheckMouseConnect (MouseDev)) {
    //
    // Send mouse reset command and set mouse default configure
    //
    Status = PS2MouseReset ();
    if (EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }

    Status = PS2MouseSetSampleRate (MouseDev->SampleRate);
    if (EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }

    Status = PS2MouseSetResolution (MouseDev->Resolution);
    if (EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }

    Status = PS2MouseSetScaling (MouseDev->Scaling);
    if (EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }

    Status = PS2MouseEnable ();
    if (EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }
  }
Exit:
  gBS->RestoreTPL (OldTpl);

  if (KeyboardEnable) {
    KbcEnableKb ();
  }

  return Status;
}

/**
  Check whether there is Ps/2 mouse device in system

  @param MouseDev   - Mouse Private Data Structure

  @retval TRUE      - Keyboard in System.
  @retval FALSE     - Keyboard not in System.

**/
BOOLEAN
CheckMouseConnect (
  IN  PS2_MOUSE_DEV     *MouseDev
  )

{
  EFI_STATUS     Status;

  Status = PS2MouseEnable ();
  if (!EFI_ERROR (Status)) {
    return TRUE;
  }

  return FALSE;
}

/**
  Get and Clear mouse status.

  @param This                 - Pointer of simple pointer Protocol.
  @param State                - Output buffer holding status.

  @retval EFI_INVALID_PARAMETER Output buffer is invalid.
  @retval EFI_NOT_READY         Mouse is not changed status yet.
  @retval EFI_SUCCESS           Mouse status is changed and get successful.
**/
EFI_STATUS
EFIAPI
MouseGetState (
  IN EFI_SIMPLE_POINTER_PROTOCOL    *This,
  IN OUT EFI_SIMPLE_POINTER_STATE   *State
  )
{
  PS2_MOUSE_DEV *MouseDev;
  EFI_TPL       OldTpl;

  MouseDev = PS2_MOUSE_DEV_FROM_THIS (This);

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!MouseDev->StateChanged) {
    return EFI_NOT_READY;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CopyMem (State, &(MouseDev->State), sizeof (EFI_SIMPLE_POINTER_STATE));

  //
  // clear mouse state
  //
  MouseDev->State.RelativeMovementX = 0;
  MouseDev->State.RelativeMovementY = 0;
  MouseDev->State.RelativeMovementZ = 0;
  MouseDev->StateChanged            = FALSE;
  gBS->RestoreTPL (OldTpl);

  return EFI_SUCCESS;
}

/**

  Event notification function for SIMPLE_POINTER.WaitForInput event.
  Signal the event if there is input from mouse.

  @param Event    event object
  @param Context  event context

**/
VOID
EFIAPI
MouseWaitForInput (
  IN  EFI_EVENT               Event,
  IN  VOID                    *Context
  )
{
  PS2_MOUSE_DEV *MouseDev;

  MouseDev = (PS2_MOUSE_DEV *) Context;

  //
  // Someone is waiting on the mouse event, if there's
  // input from mouse, signal the event
  //
  if (MouseDev->StateChanged) {
    gBS->SignalEvent (Event);
  }

}

/**
  Event notification function for TimerEvent event.
  If mouse device is connected to system, try to get the mouse packet data.

  @param Event      -  TimerEvent in PS2_MOUSE_DEV
  @param Context    -  Pointer to PS2_MOUSE_DEV structure

**/
VOID
EFIAPI
PollMouse (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )

{
  PS2_MOUSE_DEV *MouseDev;

  MouseDev = (PS2_MOUSE_DEV *) Context;

  //
  // Polling mouse packet data
  //
  PS2MouseGetPacket (MouseDev);
}
