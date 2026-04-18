// -------------------------------------------------------------------------------------------------
//  File PEVFlags.h
//
//  01/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#ifndef PEVFLAGS_HPP
#define PEVFLAGS_HPP


// -------------------------------------------------------------------------------------------------
//  Defines - Pipeline Event Flags
// -------------------------------------------------------------------------------------------------

#define IDN_PEVFLG_CHANNEL_ROUTED           0x00000001  // Channel was routed to a service/mode
#define IDN_PEVFLG_CHANNEL_CLOSED           0x00000002  // Channel is closed
#define IDN_PEVFLG_CHANNEL_SMERR            0x00000010  // Invalid serviceID or serviceMode
#define IDN_PEVFLG_CHANNEL_BSYERR           0x00000020  // Service or mode busy

#define IDN_PEVFLG_INLET_FRGERR             0x00000100  // Fragment reassembly error
#define IDN_PEVFLG_INLET_CFGERR             0x00000200  // Service mode configuration error
#define IDN_PEVFLG_INLET_CKTERR             0x00000400  // Chunk type error
#define IDN_PEVFLG_INLET_DCMERR             0x00000800  // Service data and configuration mismatch
#define IDN_PEVFLG_INLET_CTYERR             0x00001000  // Continuity error
#define IDN_PEVFLG_INLET_MCLERR             0x00002000  // Minimum chunk length error
#define IDN_PEVFLG_INLET_IAPERR             0x00008000  // Internal assertion or processing error

#define IDN_PEVFLG_OUTPUT_RGUERR            0x00010000  // Range check or unsupported feature error
#define IDN_PEVFLG_OUTPUT_PVLERR            0x00020000  // Payload validation error
#define IDN_PEVFLG_OUTPUT_DVIERR            0x00040000  // Device irregularity error
#define IDN_PEVFLG_OUTPUT_IAPERR            0x00800000  // Internal assertion or processing error



#endif
