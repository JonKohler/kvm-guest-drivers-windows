/**********************************************************************
 * Copyright (c) 2008  Red Hat, Inc.
 *
 * File: ParaNdis6-Driver.c
 *
 * This file contains implementation of NDIS6 driver envelope.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
**********************************************************************/
#include "ParaNdis6.h"
#include "ParaNdis-Oid.h"

#if NDIS60_MINIPORT

//#define NO_VISTA_POWER_MANAGEMENT

#ifdef WPP_EVENT_TRACING
#include "ParaNdis6-Driver.tmh"
#endif

static NDIS_HANDLE		DriverHandle;
static ULONG			gID = 0;

typedef PVOID (*KeRegisterProcessorChangeCallbackType) (
	__in PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
	__in PVOID CallbackContext,
	__in ULONG Flags
	);
typedef VOID (*KeDeregisterProcessorChangeCallbackType) (
	__in PVOID CallbackHandle
	);

static PVOID DummyRegisterProcessorChange (
	__in PPROCESSOR_CALLBACK_FUNCTION CallbackFunction,
	__in PVOID CallbackContext,
	__in ULONG Flags
	)
{
	KAFFINITY  afActiveProcessors = 0;
	ULONG nCount = KeQueryActiveProcessorCount(&afActiveProcessors);
	DPrintf(0, ("[%s] %d, affinity %lXh", __FUNCTION__, nCount, afActiveProcessors));
	return NULL;
}

static VOID DummyDeregisterProcessorChangeCallback(__in PVOID CallbackHandle)
{

}


static KeRegisterProcessorChangeCallbackType
	ParaNdis2008_RegisterCallback = DummyRegisterProcessorChange;

static KeDeregisterProcessorChangeCallbackType
	ParaNdis2008_DeregisterCallback = DummyDeregisterProcessorChangeCallback;

static PROCESSOR_CALLBACK_FUNCTION * ProcessorChangeCallbackHandle = NULL;

static VOID ParaNdis_OnCPUChange(
	__in PVOID CallbackContext,
	__in PKE_PROCESSOR_CHANGE_NOTIFY_CONTEXT ChangeContext,
	__inout PNTSTATUS OperationStatus
	)
{
	switch(ChangeContext->State)
	{
		case KeProcessorAddStartNotify:
			DPrintf(0, ("[%s] CPU %lXh starting", __FUNCTION__, ChangeContext->NtNumber));
			break;
		case KeProcessorAddCompleteNotify:
			DPrintf(0, ("[%s] CPU %lXh started", __FUNCTION__, ChangeContext->NtNumber));
			break;
		case KeProcessorAddFailureNotify:
			DPrintf(0, ("[%s] CPU %lXh failed to start", __FUNCTION__, ChangeContext->NtNumber));
			break;
		default:
			break;
	}
}

/**********************************************************
Produces connect indication to NDIS
Parameters:
	context
	BOOLEAN bConnected		- 1/0 connect/disconnect
***********************************************************/
void ParaNdis_IndicateConnect(PARANDIS_ADAPTER *pContext, BOOLEAN bConnected, BOOLEAN bForce)
{
	NDIS_STATUS status;
	NDIS_STATUS_INDICATION	indication;
	NDIS_LINK_STATE         state;
	DEBUG_ENTRY(3);

	if (bConnected != pContext->bConnected)
	{
		pContext->bConnected = bConnected;

		NdisZeroMemory(&state, sizeof(state));
		state.Header.Revision = NDIS_LINK_STATE_REVISION_1;
		state.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
		state.Header.Size = sizeof(state);
		state.MediaConnectState =
			pContext->bConnected ?
				MediaConnectStateConnected :
				MediaConnectStateDisconnected;
		state.MediaDuplexState = MediaDuplexStateFull;
		state.RcvLinkSpeed = state.XmitLinkSpeed =
			pContext->bConnected ?
				PARANDIS_MAXIMUM_RECEIVE_SPEED :
				NDIS_LINK_SPEED_UNKNOWN;;
		state.PauseFunctions = NdisPauseFunctionsUnsupported;

		NdisZeroMemory(&indication, sizeof(indication));

		indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
		indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
		indication.Header.Size = sizeof(indication);
		indication.SourceHandle = pContext->MiniportHandle;
		indication.StatusCode = NDIS_STATUS_LINK_STATE;
		indication.StatusBuffer = &state;
		indication.StatusBufferSize = sizeof(state);
		DPrintf(0, ("Indicating %sconnect", pContext->bConnected ? "" : "dis"));
		ParaNdis_DebugHistory(pContext, hopConnectIndication, NULL, bConnected, 0, 0);
		NdisMIndicateStatusEx(pContext->MiniportHandle , &indication);
	}
}

/**********************************************************
This is timer procedure for timer connect indication, if used
***********************************************************/
static VOID ConnectTimerCallback(
	IN PVOID  SystemSpecific1,
	IN PVOID  FunctionContext,
	IN PVOID  SystemSpecific2,
	IN PVOID  SystemSpecific3
	)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)FunctionContext;
	ParaNdis_ReportLinkStatus(pContext);
}

#define VISTA_RECOVERY_CANCEL_TIMER						1
#define VISTA_RECOVERY_RUN_DPC							2
#define VISTA_RECOVERY_INFO_ONLY_SECOND_READ			4


static UCHAR MiniportSynchronizeInterruptProcedure(PVOID  SynchronizeContext)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)SynchronizeContext;
	BOOLEAN b1;
	UCHAR val = ParaNdis_OnInterrupt(pContext, &b1);
	if (val)
	{
		// we read the interrupt, in any case run the DRC
		val = VISTA_RECOVERY_RUN_DPC;
		b1 = !VirtIODeviceISR(&pContext->IODevice);
		// if we read it again, it does not mean anything
		if (b1) val |= VISTA_RECOVERY_INFO_ONLY_SECOND_READ;
	}
	else
	{
		// we did not read the interrupt
		val = (pContext->ulIrqReceived > 1) ? VISTA_RECOVERY_CANCEL_TIMER : 0;
	}
	return val;
}



/**********************************************************
This is timer procedure for Interrupt recovery timer
***********************************************************/
static VOID InteruptRecoveryTimerCallback(
	IN PVOID  SystemSpecific1,
	IN PVOID  FunctionContext,
	IN PVOID  SystemSpecific2,
	IN PVOID  SystemSpecific3
	)
{
	UCHAR val;
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)FunctionContext;
	val = NdisMSynchronizeWithInterruptEx(
		pContext->InterruptHandle,
		0,
		MiniportSynchronizeInterruptProcedure,
		pContext);
	if (val & VISTA_RECOVERY_RUN_DPC)
		ParaNdis_DPCWorkBody(pContext);
	/* this does not happen and as such, not related to the problem
	if (val == VISTA_RECOVERY_RUN_DPC)
	{
		DPrintf(0, ("[%s] The interrupt was still set", __FUNCTION__));
	}
	*/
	if (~val & VISTA_RECOVERY_CANCEL_TIMER)
		ParaNdis_SetTimer(pContext->InterruptRecoveryTimer, 15);
	else
	{
		DPrintf(0, ("[%s] Cancelled", __FUNCTION__));
	}
	DEBUG_EXIT_STATUS(5, val);
}


/**********************************************************
Parameters:

Return value:
***********************************************************/
static NDIS_STATUS CreateTimers(PARANDIS_ADAPTER *pContext)
{
	NDIS_STATUS status;
	NDIS_TIMER_CHARACTERISTICS tch;
	DEBUG_ENTRY(4);
	tch.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
	tch.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
	tch.Header.Size = sizeof(tch);
	tch.AllocationTag = PARANDIS_MEMORY_TAG;
	tch.FunctionContext = pContext;
	tch.TimerFunction = ConnectTimerCallback;
	status = NdisAllocateTimerObject(pContext->MiniportHandle, &tch, &pContext->ConnectTimer);
	if (status == NDIS_STATUS_SUCCESS)
	{
		tch.TimerFunction = InteruptRecoveryTimerCallback;
		status = NdisAllocateTimerObject(pContext->MiniportHandle, &tch, &pContext->InterruptRecoveryTimer);
	}
	DEBUG_EXIT_STATUS(2, status);
	return status;
}


/**********************************************************
Required NDIS handler
Initialize adapter context, prepare it for operation,
set in PAUSED STATE
Return value:
	SUCCESS or kind of error
***********************************************************/
static NDIS_STATUS ParaNdis6_Initialize(
	NDIS_HANDLE miniportAdapterHandle, NDIS_HANDLE miniportDriverContext,
	PNDIS_MINIPORT_INIT_PARAMETERS miniportInitParameters)
{
	NDIS_MINIPORT_ADAPTER_ATTRIBUTES        miniportAttributes;
	NDIS_STATUS  status = NDIS_STATUS_SUCCESS;
	PARANDIS_ADAPTER *pContext;
	DEBUG_ENTRY(0);
	/* allocate context structure */
	pContext = (PARANDIS_ADAPTER *)
		NdisAllocateMemoryWithTagPriority(
			miniportAdapterHandle,
			sizeof(PARANDIS_ADAPTER),
			PARANDIS_MEMORY_TAG,
			NormalPoolPriority);
	if (!pContext)
	{
		DPrintf(0, ("[%s] ERROR: Memory allocation failed!", __FUNCTION__));
		status = NDIS_STATUS_RESOURCES;
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		/* set mandatory fields which Common use */
		NdisZeroMemory(pContext, sizeof(PARANDIS_ADAPTER));
		pContext->ulUniqueID = InterlockedIncrement(&gID);
		pContext->DriverHandle = DriverHandle;
		pContext->MiniportHandle = miniportAdapterHandle;
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		NdisZeroMemory(&miniportAttributes, sizeof(miniportAttributes));
		miniportAttributes.RegistrationAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
		miniportAttributes.RegistrationAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
		miniportAttributes.RegistrationAttributes.Header.Size = sizeof(NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES);
		miniportAttributes.RegistrationAttributes.MiniportAdapterContext = pContext;
		miniportAttributes.RegistrationAttributes.AttributeFlags =
#ifndef NO_VISTA_POWER_MANAGEMENT
			NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND |
#endif
			// actual for USB
			// NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK
			NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE |
			NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER;
		miniportAttributes.RegistrationAttributes.CheckForHangTimeInSeconds = 4;
		miniportAttributes.RegistrationAttributes.InterfaceType = NdisInterfacePci;
		status = NdisMSetMiniportAttributes(miniportAdapterHandle, &miniportAttributes);
		if (status != NDIS_STATUS_SUCCESS)
		{
			DPrintf(0, ("[%s] ERROR: NdisMSetMiniportAttributes 1 failed (%X)!", __FUNCTION__, status));
		}
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		/* set mandatory fields which Common use */
		NdisZeroMemory(pContext, sizeof(PARANDIS_ADAPTER));
		pContext->ulUniqueID = InterlockedIncrement(&gID);
		pContext->DriverHandle = DriverHandle;
		pContext->MiniportHandle = miniportAdapterHandle;
		/* prepare statistics struct for further reports */
		pContext->Statistics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
		pContext->Statistics.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
		pContext->Statistics.Header.Size = sizeof(pContext->Statistics);
		/* let Common do all the rest */
		status = ParaNdis_InitializeContext(pContext, miniportInitParameters->AllocatedResources);
		if (status != NDIS_STATUS_SUCCESS)
		{
			DPrintf(0, ("[%s] ERROR: ParaNdis6_InitializeContext failed (%X)!", __FUNCTION__, status));
		}
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		/* create timers if we'll use them */
		status = CreateTimers(pContext);
		if (status != NDIS_STATUS_SUCCESS)
		{
			DPrintf(0, ("[%s] ERROR: CreateTimers failed(%X)!", __FUNCTION__, status));
		}
	}

	if (status == NDIS_STATUS_SUCCESS)
	{
		NDIS_PNP_CAPABILITIES powerCaps;
		PNDIS_PNP_CAPABILITIES pPowerCaps = &powerCaps;
		ParaNdis_FillPowerCapabilities(&powerCaps);
#ifdef NO_VISTA_POWER_MANAGEMENT
		pPowerCaps = NULL;
#endif
		NdisZeroMemory(&miniportAttributes, sizeof(miniportAttributes));
		miniportAttributes.GeneralAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
		miniportAttributes.GeneralAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
		miniportAttributes.GeneralAttributes.Header.Size = sizeof(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES);
		miniportAttributes.GeneralAttributes.MediaType = NdisMedium802_3;
		miniportAttributes.GeneralAttributes.PhysicalMediumType = NdisPhysicalMedium802_3;
		miniportAttributes.GeneralAttributes.MtuSize = pContext->MaxPacketSize.nMaxDataSize;
		miniportAttributes.GeneralAttributes.LookaheadSize = pContext->MaxPacketSize.nMaxFullSizeOS;
		miniportAttributes.GeneralAttributes.MaxXmitLinkSpeed =
		miniportAttributes.GeneralAttributes.MaxRcvLinkSpeed  =  PARANDIS_FORMAL_LINK_SPEED;
		miniportAttributes.GeneralAttributes.MediaConnectState =
			pContext->bConnected ? MediaConnectStateConnected : MediaConnectStateDisconnected;
		miniportAttributes.GeneralAttributes.XmitLinkSpeed =
		miniportAttributes.GeneralAttributes.RcvLinkSpeed = pContext->bConnected ?
			PARANDIS_FORMAL_LINK_SPEED : NDIS_LINK_SPEED_UNKNOWN;
		miniportAttributes.GeneralAttributes.MediaDuplexState = MediaDuplexStateFull;
		miniportAttributes.GeneralAttributes.PowerManagementCapabilities = pPowerCaps;
		miniportAttributes.GeneralAttributes.MacOptions =
					NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |		/* NIC has no internal loopback support */
					NDIS_MAC_OPTION_TRANSFERS_NOT_PEND  |		/* Must be set since using  NdisMIndicateReceivePacket */
					NDIS_MAC_OPTION_NO_LOOPBACK;				/* NIC has no internal loopback support */
		if (pContext->ulPriorityVlanSetting & 1)
			miniportAttributes.GeneralAttributes.MacOptions |= NDIS_MAC_OPTION_8021P_PRIORITY;
		if (pContext->ulPriorityVlanSetting & 1)
			miniportAttributes.GeneralAttributes.MacOptions |= NDIS_MAC_OPTION_8021Q_VLAN;
		miniportAttributes.GeneralAttributes.SupportedPacketFilters = PARANDIS_PACKET_FILTERS;
		miniportAttributes.GeneralAttributes.MaxMulticastListSize = PARANDIS_MULTICAST_LIST_SIZE;
		miniportAttributes.GeneralAttributes.MacAddressLength =		ETH_LENGTH_OF_ADDRESS;
		miniportAttributes.GeneralAttributes.RecvScaleCapabilities = NULL;
		miniportAttributes.GeneralAttributes.AccessType = NET_IF_ACCESS_BROADCAST;
		miniportAttributes.GeneralAttributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
		miniportAttributes.GeneralAttributes.IfType = IF_TYPE_ETHERNET_CSMACD;
		miniportAttributes.GeneralAttributes.IfConnectorPresent = TRUE;
		miniportAttributes.GeneralAttributes.ConnectionType	= NET_IF_CONNECTION_DEDICATED;
		NdisMoveMemory(
			miniportAttributes.GeneralAttributes.PermanentMacAddress,
			pContext->PermanentMacAddress,
			ETH_LENGTH_OF_ADDRESS);
		NdisMoveMemory(
			miniportAttributes.GeneralAttributes.CurrentMacAddress,
			pContext->CurrentMacAddress,
			ETH_LENGTH_OF_ADDRESS);
		ParaNdis6_GetSupportedOid(&miniportAttributes.GeneralAttributes);
		/* update also SupportedStatistics in ready to use statistics block */
		pContext->Statistics.SupportedStatistics = ParaNdis6_GetSupportedStatisticsFlags();
		status = NdisMSetMiniportAttributes(miniportAdapterHandle, &miniportAttributes);
		if (status != NDIS_STATUS_SUCCESS)
		{
			DPrintf(0, ("[%s] ERROR: NdisMSetMiniportAttributes 2 failed (%X)!", __FUNCTION__, status));
		}
	}

	if (pContext && status != NDIS_STATUS_SUCCESS && status != NDIS_STATUS_PENDING)
	{
		// no need to cleanup
		NdisFreeMemory(pContext, 0, 0);
	}
	if (pContext && status == NDIS_STATUS_SUCCESS)
	{
		status = ParaNdis_FinishInitialization(pContext);
		if (status != NDIS_STATUS_SUCCESS)
		{
			ParaNdis_CleanupContext(pContext);
			NdisFreeMemory(pContext, 0, 0);
		}
	}
	if (pContext && status == NDIS_STATUS_SUCCESS)
	{
		if (NDIS_STATUS_SUCCESS ==
			ParaNdis6_GetRegistrationOffloadInfo(pContext,
				&miniportAttributes.OffloadAttributes))
			status = NdisMSetMiniportAttributes(miniportAdapterHandle, &miniportAttributes);
			if (status != NDIS_STATUS_SUCCESS)
			{
				DPrintf(0, ("[%s] ERROR: NdisMSetMiniportAttributes 3 failed (%X)!", __FUNCTION__, status));
			}
	}

	if (pContext && status == NDIS_STATUS_SUCCESS)
	{
		ParaNdis_DebugRegisterMiniport(pContext, TRUE);
		if (pContext->ulMilliesToConnect)
			ParaNdis_SetTimer(pContext->ConnectTimer, pContext->ulMilliesToConnect);
		if (pContext->bDoInterruptRecovery)
			ParaNdis_SetTimer(pContext->InterruptRecoveryTimer, 15); //15 mSec is the recovery timeout
	}
	DEBUG_EXIT_STATUS(status ? 0 : 2, status);
	return status;
}


/**********************************************************
called at IRQL = PASSIVE_LEVEL
Called on disable, on removal, on standby (if required)
***********************************************************/
static VOID ParaNdis6_Halt(NDIS_HANDLE miniportAdapterContext, NDIS_HALT_ACTION haltAction)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	DEBUG_ENTRY(0);
	ParaNdis_DebugHistory(pContext, hopHalt, NULL, 1, haltAction, 0);
	if (pContext->InterruptRecoveryTimer)
	{
		NdisCancelTimerObject(pContext->InterruptRecoveryTimer);
		NdisFreeTimerObject(pContext->InterruptRecoveryTimer);
	}
	if (pContext->ConnectTimer)
	{
		NdisCancelTimerObject(pContext->ConnectTimer);
		NdisFreeTimerObject(pContext->ConnectTimer);
	}
	ParaNdis_CleanupContext(pContext);
	ParaNdis_DebugHistory(pContext, hopHalt, NULL, 0, 0, 0);
	ParaNdis_DebugRegisterMiniport(pContext, FALSE);
	NdisFreeMemory(pContext, 0, 0);
	DEBUG_EXIT_STATUS(2, 0);
}

/**********************************************************
Deregister driver
Clean up WPP
***********************************************************/
static VOID ParaNdis6_Unload(IN PDRIVER_OBJECT pDriverObject)
{
	DEBUG_ENTRY(0);
	ParaNdis2008_DeregisterCallback(ProcessorChangeCallbackHandle);
	if (DriverHandle) NdisMDeregisterMiniportDriver(DriverHandle);
	DEBUG_EXIT_STATUS(2, 0);
	ParaNdis_DebugCleanup(pDriverObject);
}

/**********************************************************
	callback from asynchronous RECEIVE PAUSE
***********************************************************/
static void OnReceivePauseComplete(PARANDIS_ADAPTER *pContext)
{
	DEBUG_ENTRY(0);
	// pause exit
	ParaNdis_DebugHistory(pContext, hopSysPause, NULL, 0, 0, 0);
	NdisMPauseComplete(pContext->MiniportHandle);
}

/**********************************************************
	callback from asynchronous SEND PAUSE
***********************************************************/
static void OnSendPauseComplete(PARANDIS_ADAPTER *pContext)
{
	NDIS_STATUS  status;
	DEBUG_ENTRY(0);
	status = ParaNdis6_ReceivePauseRestart(pContext, TRUE, OnReceivePauseComplete);
	if (status != NDIS_STATUS_PENDING)
	{
		// pause exit
		ParaNdis_DebugHistory(pContext, hopSysPause, NULL, 0, 0, 0);
		NdisMPauseComplete(pContext->MiniportHandle);
	}
}


/**********************************************************
Required NDIS handler
called at IRQL = PASSIVE_LEVEL
Must pause RX and TX
Called before halt, on standby,
upon protocols activation
***********************************************************/
static NDIS_STATUS ParaNdis6_Pause(
		NDIS_HANDLE							miniportAdapterContext,
		PNDIS_MINIPORT_PAUSE_PARAMETERS     miniportPauseParameters)
{
	NDIS_STATUS  status;
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	DEBUG_ENTRY(0);
	ParaNdis_DebugHistory(pContext, hopSysPause, NULL, 1, 0, 0);
	status = ParaNdis6_SendPauseRestart(pContext, TRUE, OnSendPauseComplete);
	if (status != NDIS_STATUS_PENDING)
	{
		status = ParaNdis6_ReceivePauseRestart(pContext, TRUE, OnReceivePauseComplete);
	}
	DEBUG_EXIT_STATUS(0, status);
	if (status == STATUS_SUCCESS)
	{
		// pause exit
		ParaNdis_DebugHistory(pContext, hopSysPause, NULL, 0, 0, 0);
	}
	return status;
}

/**********************************************************
Required NDIS handler
called at IRQL = PASSIVE_LEVEL
Must pause RX and TX
Called upon startup, on resume,
upon protocols activation
***********************************************************/
static NDIS_STATUS ParaNdis6_Restart(
	NDIS_HANDLE							miniportAdapterContext,
	PNDIS_MINIPORT_RESTART_PARAMETERS   miniportRestartParameters)
{
	NDIS_STATUS  status = NDIS_STATUS_SUCCESS;
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	DEBUG_ENTRY(0);
	ParaNdis_DebugHistory(pContext, hopSysResume, NULL, 1, 0, 0);
	ParaNdis6_SendPauseRestart(pContext, FALSE, NULL);
	ParaNdis6_ReceivePauseRestart(pContext, FALSE, NULL);
	if (!pContext->ulMilliesToConnect)
	{
		ParaNdis_ReportLinkStatus(pContext);
	}
	ParaNdis_DebugHistory(pContext, hopSysResume, NULL, 0, 0, 0);
	DEBUG_EXIT_STATUS(2, status);
	return status;
}


/**********************************************************
Required NDIS handler
called at IRQL <= DISPATCH_LEVEL
***********************************************************/
static VOID ParaNdis6_SendNetBufferLists(
	NDIS_HANDLE miniportAdapterContext,
	PNET_BUFFER_LIST    pNBL,
	NDIS_PORT_NUMBER    portNumber,
	ULONG               sendFlags)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	ParaNdis6_Send(pContext, pNBL, !!(sendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL));
}

/**********************************************************
Required NDIS handler: happens unually each 2 second
***********************************************************/
static BOOLEAN ParaNdis6_CheckForHang(
	NDIS_HANDLE miniportAdapterContext)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	return ParaNdis_CheckForHang(pContext);
}


/**********************************************************
	callback from asynchronous SEND PAUSE on reset
***********************************************************/
static void OnSendPauseCompleteOnReset(PARANDIS_ADAPTER *pContext)
{
	NDIS_STATUS  status;
	DEBUG_ENTRY(0);
	NdisSetEvent(&pContext->ResetEvent);
}

/**********************************************************
	callback from asynchronous RECEIVE PAUSE on reset
***********************************************************/
static void OnReceivePauseCompleteOnReset(PARANDIS_ADAPTER *pContext)
{
	NDIS_STATUS  status;
	DEBUG_ENTRY(0);
	NdisSetEvent(&pContext->ResetEvent);
}

VOID ParaNdis_Suspend(PARANDIS_ADAPTER *pContext)
{
	DEBUG_ENTRY(0);
	NdisResetEvent(&pContext->ResetEvent);
	if (NDIS_STATUS_PENDING != ParaNdis6_SendPauseRestart(pContext, TRUE, OnSendPauseCompleteOnReset))
	{
		NdisSetEvent(&pContext->ResetEvent);
	}
	NdisWaitEvent(&pContext->ResetEvent, 0);
	NdisResetEvent(&pContext->ResetEvent);
	if (NDIS_STATUS_PENDING != ParaNdis6_ReceivePauseRestart(pContext, TRUE, OnReceivePauseCompleteOnReset))
	{
		NdisSetEvent(&pContext->ResetEvent);
	}
	NdisWaitEvent(&pContext->ResetEvent, 0);
	DEBUG_EXIT_STATUS(0, 0);
}

static void OnResetWorkItem(PVOID  WorkItemContext, NDIS_HANDLE  NdisIoWorkItemHandle)
{
	tGeneralWorkItem *pwi = (tGeneralWorkItem *)WorkItemContext;
	PARANDIS_ADAPTER *pContext = pwi->pContext;
	BOOLEAN bSendActive, bReceiveActive;
	DEBUG_ENTRY(0);
	bSendActive = pContext->SendState == srsEnabled;
	bReceiveActive = pContext->ReceiveState == srsEnabled;
	if (!pContext->bDoHardReset)
	{
		ParaNdis_IndicateConnect(pContext, FALSE, FALSE);
		ParaNdis_Suspend(pContext);
		if (bSendActive) ParaNdis6_SendPauseRestart(pContext, FALSE, NULL);
		if (bReceiveActive) ParaNdis6_ReceivePauseRestart(pContext, FALSE, NULL);
		ParaNdis_ReportLinkStatus(pContext);
	}
	else
	{
		ParaNdis_PowerOff(pContext);
		ParaNdis_PowerOn(pContext);
		if (bSendActive) ParaNdis6_SendPauseRestart(pContext, FALSE, NULL);
		if (bReceiveActive) ParaNdis6_ReceivePauseRestart(pContext, FALSE, NULL);
	}
	NdisFreeMemory(pwi, 0, 0);
	NdisFreeIoWorkItem(NdisIoWorkItemHandle);
	ParaNdis_DebugHistory(pContext, hopSysReset, NULL, 0, NDIS_STATUS_SUCCESS, 0);
	NdisMResetComplete(pContext->MiniportHandle, NDIS_STATUS_SUCCESS, TRUE);
}


/**********************************************************
Required NDIS handler for RESET operation
Never happens under normal condition, only if
OID or other call returns PENDING and not completed or if
ParaNdis6_CheckForHang returns true
***********************************************************/
static NDIS_STATUS ParaNdis6_Reset(
		NDIS_HANDLE miniportAdapterContext,
		PBOOLEAN  pAddressingReset)
{
	NDIS_STATUS  status = NDIS_STATUS_FAILURE;
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	NDIS_HANDLE hwo;
	tGeneralWorkItem *pwi;
	DEBUG_ENTRY(0);
	ParaNdis_DebugHistory(pContext, hopSysReset, NULL, 1, 0, 0);
	hwo = NdisAllocateIoWorkItem(pContext->MiniportHandle);
	pwi = ParaNdis_AllocateMemory(pContext, sizeof(tGeneralWorkItem));
	if (pwi && hwo)
	{
		pwi->pContext = pContext;
		pwi->WorkItem = hwo;
		NdisQueueIoWorkItem(hwo, OnResetWorkItem, pwi);
		status = NDIS_STATUS_PENDING;
	}
	else
	{
		if (pwi) NdisFreeMemory(pwi, 0, 0);
		if (hwo) NdisFreeIoWorkItem(hwo);
		ParaNdis_DebugHistory(pContext, hopSysReset, NULL, 0, status, 0);
	}
	DEBUG_EXIT_STATUS(0, status);
	return status;
}

VOID ParaNdis_Resume(PARANDIS_ADAPTER *pContext)
{
	/* nothing to do */
	DEBUG_EXIT_STATUS(0, 0);
}


/***************************************************
Required NDIS handler for adapter shutdown
should not call NDIS functions
The reason may be a system shutdown ( IRQL <= DPC)
may be bugcheck (arbitrary IRQL)
***************************************************/
static VOID ParaNdis6_AdapterShutdown(
	NDIS_HANDLE miniportAdapterContext,
	NDIS_SHUTDOWN_ACTION  shutdownAction)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	ParaNdis_OnShutdown(pContext);
}

/**********************************************************
Required NDIS handler for PnP event
***********************************************************/
static VOID ParaNdis6_DevicePnPEvent(
	NDIS_HANDLE miniportAdapterContext,
	PNET_DEVICE_PNP_EVENT pNetEvent)
{
	PARANDIS_ADAPTER *pContext = (PARANDIS_ADAPTER *)miniportAdapterContext;
	ParaNdis_OnPnPEvent(pContext,
		pNetEvent->DevicePnPEvent,
		pNetEvent->InformationBuffer,
		pNetEvent->InformationBufferLength);

}

static NDIS_STATUS  ParaNdis6_AddDevice(IN NDIS_HANDLE  MiniportAdapterHandle, IN NDIS_HANDLE  MiniportDriverContext)
{
	NDIS_MINIPORT_ADAPTER_ATTRIBUTES  MiniportAttributes;
	NDIS_STATUS status;
	DEBUG_ENTRY(0);
	MiniportAttributes.AddDeviceRegistrationAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES;
	MiniportAttributes.AddDeviceRegistrationAttributes.Header.Revision = NDIS_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES_REVISION_1;
	MiniportAttributes.AddDeviceRegistrationAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES_REVISION_1;
	MiniportAttributes.AddDeviceRegistrationAttributes.MiniportAddDeviceContext = MiniportAdapterHandle;
	MiniportAttributes.AddDeviceRegistrationAttributes.Flags = 0;
	status = NdisMSetMiniportAttributes(MiniportAdapterHandle, &MiniportAttributes);
	return status;
}

static VOID ParaNdis6_RemoveDevice (IN NDIS_HANDLE  MiniportAddDeviceContext)
{
	DEBUG_ENTRY(0);
}

static NDIS_STATUS ParaNdis6_StartDevice(IN NDIS_HANDLE  MiniportAddDeviceContext, IN PIRP  Irp)
{
	NDIS_STATUS status = NDIS_STATUS_SUCCESS;
	DEBUG_ENTRY(0);
	return status;
}

typedef struct _tagRRLData
{
	PIO_RESOURCE_REQUIREMENTS_LIST prrl;
	PIO_RESOURCE_LIST currentList;
	PIO_RESOURCE_DESCRIPTOR currentDesc;
}tRRLData;

/******************************************************************
Replacement of resource requirement list: initialize the new list
*******************************************************************/
static void InitializeNewResourceRequirementsList(
	tRRLData *pData,
	PIO_RESOURCE_REQUIREMENTS_LIST newList,
	PIO_RESOURCE_REQUIREMENTS_LIST oldList)
{
	pData->prrl = newList;
	pData->currentList = NULL;
	pData->currentDesc = NULL;
	if (pData->prrl)
	{
		ULONG len = RtlPointerToOffset(pData->prrl, &pData->prrl->List[0]);
		RtlCopyMemory(newList, oldList, len);
		newList->ListSize = len;
		newList->AlternativeLists = 0;
	}
}

/******************************************************************
Replacement of resource requirement list: adding new resource list
to existing resource requirement list
*******************************************************************/
static void AddNewResourceList(tRRLData *pData, PIO_RESOURCE_LIST pior)
{
	if (pData->prrl)
	{
		ULONG len = RtlPointerToOffset(pior, &pior->Descriptors[0]);
		pData->currentList = (PIO_RESOURCE_LIST)RtlOffsetToPointer(pData->prrl, pData->prrl->ListSize);
		RtlCopyMemory(pData->currentList, pior, len);
		pData->currentList->Count = 0;
		pData->prrl->ListSize += len;
		pData->prrl->AlternativeLists++;
		pData->currentDesc = &pData->currentList->Descriptors[0];
	}
}

/******************************************************************
Replacement of resource requirement list: done with new resource list,
verify if it contains all the required resources
*******************************************************************/
static void FinalizeResourceList(tRRLData *pData)
{
	if (pData->prrl && pData->currentList)
	{
		BOOLEAN bFound = FALSE;
		ULONG len = RtlPointerToOffset(pData->currentList, &pData->currentList->Descriptors[0]);
		UINT i;
		for (i = 0; i < pData->currentList->Count && !bFound; ++i)
		{
			len += sizeof(IO_RESOURCE_DESCRIPTOR);
			if (pData->currentList->Descriptors[i].Type == CmResourceTypeInterrupt) bFound = TRUE;
		}
		if (!bFound)
		{
			pData->prrl->AlternativeLists--;
			pData->prrl->ListSize -= len;
		}
	}
}


/******************************************************************
Replacement of resource requirement list: adding new resource descriptor
to current resource list
*******************************************************************/
static void AddNewResourceDescriptor(tRRLData *pData, PIO_RESOURCE_DESCRIPTOR prd)
{
	if (pData->prrl && pData->currentList && pData->currentDesc)
	{
		*(pData->currentDesc) = *prd;
		pData->currentList->Count++;
		pData->prrl->ListSize += sizeof(IO_RESOURCE_DESCRIPTOR);
		pData->currentDesc++;
	}
}


/******************************************************************
Replacement of resource requirement list, when needed:
The procedure traverses over all the resource lists in existing resource requirement list
(we receive it in IRP information field).
When the driver is not built to work with MSI resources, we must remove them from the
resource requirement list, otherwise the driver will fail to initialize
Typically MSI interrupts are labeled as preferred ones, when line interrupts are labeled as
alternative resources. Removing message interrupts, remove also "alternative" label from line interrupts.
*******************************************************************/
static PIO_RESOURCE_REQUIREMENTS_LIST ParseFilterResourceIrp(
	IN NDIS_HANDLE  MiniportAddDeviceContext,
	PIO_RESOURCE_REQUIREMENTS_LIST prrl,
	BOOLEAN bRemoveMSIResources)
{
	tRRLData newRRLData;
	PIO_RESOURCE_REQUIREMENTS_LIST newPrrl = NULL;
	DPrintf(0, ("[%s]%s", __FUNCTION__, bRemoveMSIResources ? "(Remove MSI resources...)" : ""));
	if (MiniportAddDeviceContext && prrl) newPrrl = (PIO_RESOURCE_REQUIREMENTS_LIST)NdisAllocateMemoryWithTagPriority(
			MiniportAddDeviceContext,
			prrl->ListSize,
			PARANDIS_MEMORY_TAG,
			NormalPoolPriority);
	InitializeNewResourceRequirementsList(&newRRLData, newPrrl, prrl);
	if (prrl)
	{
		ULONG n, offset;
		PVOID p = &prrl->List[0];
		DPrintf(0, ("[%s] %d bytes, %d lists", __FUNCTION__, prrl->ListSize, prrl->AlternativeLists));
		offset = RtlPointerToOffset(prrl, p);
		for (n = 0; n < prrl->AlternativeLists && offset < prrl->ListSize; ++n)
		{
			ULONG nDesc;
			IO_RESOURCE_LIST *pior = (IO_RESOURCE_LIST *)p;
			if ((offset + sizeof(*pior)) < prrl->ListSize)
			{
				IO_RESOURCE_DESCRIPTOR *pd = &pior->Descriptors[0];
				DPrintf(0, ("[%s]+%d %d:%d descriptors follow", __FUNCTION__, offset, n, pior->Count));
				offset += RtlPointerToOffset(p, pd);
				AddNewResourceList(&newRRLData, pior);
				for (nDesc = 0; nDesc < pior->Count; ++nDesc)
				{
					BOOLEAN bRemove = FALSE;
					if ((offset + sizeof(*pd)) <= prrl->ListSize)
					{
						DPrintf(0, ("[%s]+%d %d: type %d, flags %X, option %X",
							__FUNCTION__, offset, nDesc, pd->Type, pd->Flags, pd->Option));
						if (pd->Type == CmResourceTypeInterrupt)
						{
							if (pd->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
							{
								bRemove = bRemoveMSIResources;
							}
							else
							{
								// reset IO_RESOURCE_ALTERNATIVE attribute on Line Interrupt,
								// if we remove MSI vectors, otherwise Windows will not allocate it for the device
								if (bRemoveMSIResources && (pd->Option & IO_RESOURCE_ALTERNATIVE))
								{
									pd->Option &= ~IO_RESOURCE_ALTERNATIVE;
								}
							}
						}
						if (!bRemove) AddNewResourceDescriptor(&newRRLData, pd);
					}
					offset += sizeof(*pd);
					pd = (IO_RESOURCE_DESCRIPTOR *)RtlOffsetToPointer(prrl, offset);
				}
				FinalizeResourceList(&newRRLData);
				p = pd;
			}
		}
	}

	return newPrrl;
}

/******************************************************************
Registered procedure for filtering if resource requirement
Required when the device supports MSI, but the driver decides - will it work with MSI or not
(currently MSI is supported but does not work, exactly our case).
In this case the resource requirement list must be replaced - we need to remove from it
all the "message interrupt" resources.

When we are ready to work with MSI (VIRTIO_USE_MSIX_INTERRUPT is DEFINED),
we just enumerate allocated resources and do not modify them.
*******************************************************************/
static NDIS_STATUS ParaNdis6_FilterResource(IN NDIS_HANDLE  MiniportAddDeviceContext, IN PIRP  Irp)
{
	BOOLEAN bRemoveMSI = FALSE;
	PIO_RESOURCE_REQUIREMENTS_LIST prrl = (PIO_RESOURCE_REQUIREMENTS_LIST)(PVOID)Irp->IoStatus.Information;
#if !defined(VIRTIO_USE_MSIX_INTERRUPT)
	bRemoveMSI = TRUE;
#endif
	if (bRemoveMSI)
	{
		// traverse the resource requirements list, clean up all the MSI resources
		PIO_RESOURCE_REQUIREMENTS_LIST newPrrl = ParseFilterResourceIrp(MiniportAddDeviceContext, prrl, TRUE);
		if (newPrrl)
		{
			Irp->IoStatus.Information = (ULONG_PTR)newPrrl;
			NdisFreeMemory(prrl, 0, 0);
			DPrintf(0, ("[%s] Reparsing resources using new requirements...", __FUNCTION__));
			// just parse and print after MSI cleanup, this time do not remove amything and do not reallocate the list
			ParseFilterResourceIrp(NULL, newPrrl, FALSE);
		}
	}
	else
	{
		// just parse and print, do not remove amything and do not reallocate the list
		ParseFilterResourceIrp(NULL, prrl, FALSE);
	}
	return NDIS_STATUS_SUCCESS;
}



/******************************************************************************
This procedure required when we want to be able filtering resource requirements.
ParaNdis6_AddDevice need to register context (to allow other procedures to allocate memory)
ParaNdis6_FilterResource able to replace resource requirements list if needed
ParaNdis6_RemoveDevice does not do anything if other procedures do not allocate any resources
 which must be freed upon device removal
******************************************************************************/
static NDIS_STATUS ParaNdis6_SetOptions(IN  NDIS_HANDLE NdisDriverHandle, IN  NDIS_HANDLE DriverContext)
{
	NDIS_STATUS status;
	NDIS_MINIPORT_PNP_CHARACTERISTICS pnpChars;
	pnpChars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_PNP_CHARACTERISTICS;
	pnpChars.Header.Revision = NDIS_MINIPORT_PNP_CHARACTERISTICS_REVISION_1;
	pnpChars.Header.Size = NDIS_SIZEOF_MINIPORT_PNP_CHARACTERISTICS_REVISION_1;
	pnpChars.MiniportAddDeviceHandler = ParaNdis6_AddDevice;
	pnpChars.MiniportRemoveDeviceHandler = ParaNdis6_RemoveDevice;
	pnpChars.MiniportStartDeviceHandler = ParaNdis6_StartDevice;
	pnpChars.MiniportFilterResourceRequirementsHandler = ParaNdis6_FilterResource;
	status = NdisSetOptionalHandlers(NdisDriverHandle, (PNDIS_DRIVER_OPTIONAL_HANDLERS)&pnpChars);
	return status;
}

/**********************************************************
Driver entry point:
Register miniport driver
Initialize WPP
Return value:
	status of driver registration
***********************************************************/
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	NDIS_STATUS                             status = NDIS_STATUS_FAILURE;
	NDIS_MINIPORT_DRIVER_CHARACTERISTICS    chars;

	ParaNdis_DebugInitialize(pDriverObject, pRegistryPath);

	DEBUG_ENTRY(0);
	_LogOutString(0, __DATE__ " " __TIME__);
	NdisZeroMemory(&chars, sizeof(chars));

	chars.Header.Type      = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
	chars.Header.Size      = sizeof(NDIS_MINIPORT_DRIVER_CHARACTERISTICS);
	chars.Header.Revision  = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1;

	chars.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
	chars.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
	/* stupid thing, they are at least short */
	chars.MajorDriverVersion = (UCHAR)PARANDIS_MAJOR_DRIVER_VERSION;
	chars.MinorDriverVersion = (UCHAR)PARANDIS_MINOR_DRIVER_VERSION;

	// possible value for regular miniport NDIS_WDM_DRIVER - for USB or 1394
	// chars.Flags	= 0;

	chars.InitializeHandlerEx			= ParaNdis6_Initialize;
	chars.HaltHandlerEx					= ParaNdis6_Halt;
	chars.UnloadHandler					= ParaNdis6_Unload;
	chars.PauseHandler					= ParaNdis6_Pause;
	chars.RestartHandler				= ParaNdis6_Restart;
	chars.OidRequestHandler				= ParaNdis6_OidRequest;
	chars.CancelOidRequestHandler		= ParaNdis6_OidCancelRequest;
	chars.SendNetBufferListsHandler		= ParaNdis6_SendNetBufferLists;
	chars.CancelSendHandler				= ParaNdis6_CancelSendNetBufferLists;
	chars.ReturnNetBufferListsHandler	= ParaNdis6_ReturnNetBufferLists;
	chars.CheckForHangHandlerEx			= ParaNdis6_CheckForHang;
	chars.ResetHandlerEx				= ParaNdis6_Reset;
	chars.ShutdownHandlerEx				= ParaNdis6_AdapterShutdown;
	chars.DevicePnPEventNotifyHandler	= ParaNdis6_DevicePnPEvent;
	chars.SetOptionsHandler				= ParaNdis6_SetOptions;
	status = NdisMRegisterMiniportDriver(
			pDriverObject,
			pRegistryPath,
			NULL,
			&chars,
			&DriverHandle);

	if (status == NDIS_STATUS_SUCCESS)
	{
		NDIS_STRING usRegister, usDeregister;
		PVOID pr, pd;
		NdisInitUnicodeString(&usRegister, L"KeRegisterProcessorChangeCallback");
		NdisInitUnicodeString(&usDeregister, L"KeDeregisterProcessorChangeCallback");
		pr = MmGetSystemRoutineAddress(&usRegister);
		pd = MmGetSystemRoutineAddress(&usDeregister);
		if (pr && pd)
		{
			ParaNdis2008_RegisterCallback = (KeRegisterProcessorChangeCallbackType)pr;
			ParaNdis2008_DeregisterCallback = (KeDeregisterProcessorChangeCallbackType)pd;
		}
		ProcessorChangeCallbackHandle = ParaNdis2008_RegisterCallback(
			ParaNdis_OnCPUChange,
			NULL,
			KE_PROCESSOR_CHANGE_ADD_EXISTING);
	}
	DEBUG_EXIT_STATUS(status ? 0 : 4, status);
	return status;
}

#endif //NDIS60_MINIPORT
