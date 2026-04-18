// -------------------------------------------------------------------------------------------------
//  File ODFTaxiBuffer.hpp
//
//  OpenIDN DAC Framework taxi buffer for network interface input/processing
//
//  04/2025 Dirk Apitz, created
// -------------------------------------------------------------------------------------------------


#if defined ODF_USE_TAXI_SOCK
#include "../stage/SockTaxiBuffer.hpp"
#elif defined ODF_USE_TAXI_LRAW
#include "../stage/LRawTaxiBuffer.hpp"
#else
#error "No taxi buffer option"
#endif

