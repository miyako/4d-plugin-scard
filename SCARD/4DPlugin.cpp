/* --------------------------------------------------------------------------------
 #
 #	4DPlugin.cpp
 #	source generated by 4D Plugin Wizard
 #	Project : SCARD
 #	author : miyako
 #	2018/04/17
 #
 # --------------------------------------------------------------------------------*/


#include "4DPluginAPI.h"
#include "4DPlugin.h"

/* 
 * start smart card services on windows if SCARD_E_NO_SERVICE is returned
 */

LONG SCardEstablishContextEx(DWORD dwScope, LPCVOID pvReserved1, LPCVOID pvReserved2, LPSCARDCONTEXT phContext)
{
	LONG lResult = SCardEstablishContext(dwScope, pvReserved1, pvReserved2, phContext);
#if VERSIONWIN
	if(lResult == SCARD_E_NO_SERVICE)
	{
		HANDLE hEvent = SCardAccessStartedEvent();
		DWORD dwResult = WaitForSingleObject(hEvent, 5000);
		if (dwResult == WAIT_OBJECT_0)
		{
			lResult = SCardEstablishContext(dwScope, pvReserved1, pvReserved2, phContext);
		}
		SCardReleaseStartedEvent();
	}
#endif
	return lResult;
}

#pragma mark JSON

#ifndef NOJSON	
void json_stringify(JSONNODE *json, C_TEXT &t)
{
	//json_char *json_string = json_write_formatted(json);
	json_char *json_string = json_write(json);
	std::wstring wstr = std::wstring(json_string);
	
#if VERSIONWIN
	t.setUTF16String((const PA_Unichar *)wstr.c_str(), (uint32_t)wstr.length());
#else
	uint32_t dataSize = (uint32_t)((wstr.length() * sizeof(wchar_t))+ sizeof(PA_Unichar));
	std::vector<char> buf(dataSize);
	uint32_t len = PA_ConvertCharsetToCharset((char *)wstr.c_str(),
																						(PA_long32)(wstr.length() * sizeof(wchar_t)),
																						eVTC_UTF_32,
																						(char *)&buf[0],
																						dataSize,
																						eVTC_UTF_16);
	t.setUTF16String((const PA_Unichar *)&buf[0], len);
#endif
	json_free(json_string);
}

void json_set_s(JSONNODE *n, const wchar_t *name, C_TEXT& value)
{
	if(n)
	{
		std::wstring u32;
#if VERSIONWIN
		u32 = std::wstring((wchar_t *)value.getUTF16StringPtr());
#else
		
		uint32_t dataSize = (value.getUTF16Length() * sizeof(wchar_t))+ sizeof(wchar_t);
		std::vector<char> buf(dataSize);
		
		PA_ConvertCharsetToCharset((char *)value.getUTF16StringPtr(),
															 value.getUTF16Length() * sizeof(PA_Unichar),
															 eVTC_UTF_16,
															 (char *)&buf[0],
															 dataSize,
															 eVTC_UTF_32);
		
		u32 = std::wstring((wchar_t *)&buf[0]);
#endif
		json_push_back(n, json_new_a(name, u32.c_str()));
	}
}

void json_set_i(JSONNODE *n, const wchar_t *name, C_LONGINT& value)
{
	if(n)
	{
		json_push_back(n, json_new_i(name, value.getIntValue()));
	}
}
#endif

#pragma mark -

void PluginMain(PA_long32 selector, PA_PluginParameters params)
{
	try
	{
		PA_long32 pProcNum = selector;
		sLONG_PTR *pResult = (sLONG_PTR *)params->fResult;
		PackagePtr pParams = (PackagePtr)params->fParameters;

		CommandDispatcher(pProcNum, pResult, pParams); 
	}
	catch(...)
	{

	}
}

void CommandDispatcher (PA_long32 pProcNum, sLONG_PTR *pResult, PackagePtr pParams)
{
	switch(pProcNum)
	{
// --- SCARD

		case 1 :
			SCARD_Get_info(pResult, pParams);
			break;

		case 2 :
			SCARD_READER_LIST(pResult, pParams);
			break;

	}
}

#pragma mark -

// ------------------------------------- SCARD ------------------------------------

LONG getData(SCARDHANDLE hCard,
					 const SCARD_IO_REQUEST *pioSendPci,
					 LPCBYTE pbSendBuffer,
					 DWORD cbSendLength,
					 SCARD_IO_REQUEST *pioRecvPci,
					 LPBYTE pbRecvBuffer,
					 LPDWORD pcbRecvLength,
					 C_TEXT& Param,
					 getDataMode mode)
{
	C_BLOB temp;
	
	/* default values */
	switch (mode)
	{
		case getDataMode_Type:
		{
			uint8_t cardType = 0L;/* unknown */
			temp.setBytes(&cardType, 1);
			temp.toHexText(&Param);
		}
			break;
		default:
			break;
	}
	
	LONG lResult;
	
	lResult = SCardTransmit(hCard,
													pioSendPci,
													pbSendBuffer,
													cbSendLength,
													pioRecvPci,
													pbRecvBuffer,
													pcbRecvLength);
	switch(lResult)
	{
		case SCARD_S_SUCCESS:
		{
			DWORD cbRecvLength = *pcbRecvLength;
			
			BYTE SW1 = pbRecvBuffer[cbRecvLength - 2];
			BYTE SW2 = pbRecvBuffer[cbRecvLength - 1];
			
			if ( SW1 != 0x90 || SW2 != 0x00 )
			{
				if ( SW1 == 0x63 && SW2 == 0x00 )
				{
					/* data is not available */
				}
			}
			else
			{
				switch (mode)
				{
					case getDataMode_ID:
							temp.setBytes((const uint8_t *)pbRecvBuffer, cbRecvLength-2);
							temp.toHexText(&Param);
						break;
					case getDataMode_Type:
					{
						uint8_t cardType = pbRecvBuffer[0];
						temp.setBytes(&cardType, 1);
						temp.toHexText(&Param);
					}
						break;
					case getDataMode_Sys:
					{
						if (cbRecvLength - 2 != 19 || pbRecvBuffer[0] != 0x01)
						{
							lResult = SCARD_F_INTERNAL_ERROR;
						}else
						{
							temp.setBytes((const uint8_t *)&pbRecvBuffer[cbRecvLength - 4], 2);
							temp.toHexText(&Param);
						}
					}
						break;
					case getDataMode_Name:
					{
#ifdef _WIN32
						wchar_t	buf[MAXIMUM_NAME_STRING_LENGTH];
						int len = MultiByteToWideChar(CP_ACP, 0, (LPCCH)pbRecvBuffer, cbRecvLength - 2, (LPWSTR)buf, MAXIMUM_NAME_STRING_LENGTH);
						if(len)
						{
							Param.setUTF16String((const PA_Unichar*)buf, len);
						}
#endif
					}
					default:
						break;
				}
			}
		}
			break;
		case 0x458:
			lResult = SCARD_W_REMOVED_CARD;
			break;
		case 0x16:
			lResult = SCARD_E_INVALID_PARAMETER;
			break;
		default:
			
			break;
	}
	
	return lResult;
}

void SCARD_Get_info(sLONG_PTR *pResult, PackagePtr pParams)
{
	C_TEXT Param1_reader;
	C_LONGINT Param2_mode;
	C_LONGINT Param3_protocols;
	C_LONGINT Param4_timeout;
	C_LONGINT Param5_error;
	C_TEXT returnValue;
	
	Param1_reader.fromParamAtIndex(pParams, 1);
	Param2_mode.fromParamAtIndex(pParams, 2);
	Param3_protocols.fromParamAtIndex(pParams, 3);
	Param4_timeout.fromParamAtIndex(pParams, 4);
	
	/* params in */
	DWORD timeout = Param4_timeout.getIntValue();
	DWORD protocols = Param3_protocols.getIntValue();
	DWORD mode = Param2_mode.getIntValue();

	/* params out */
	C_LONGINT Param_state;
	C_TEXT Param_atr;
	C_TEXT Param_IDm;
	C_TEXT Param_PMm;
	C_TEXT Param_Type;
	C_TEXT Param_CID;
	C_TEXT Param_Name;
	C_TEXT Param_TypeName;
	C_TEXT Param_SystemCode;

	if(!(protocols & SCARD_PROTOCOL_T0) && !(protocols & SCARD_PROTOCOL_T1))
		protocols = DEFAULT_PROTOCOLS;
	
	if(timeout == 0)
		timeout = DEFAULT_TIMEOUT_MS;
	
	if(timeout == INFINITE)
		timeout = MAX_TIMEOUT_MS;

	if((mode != SCARD_SHARE_SHARED) && (mode != SCARD_SHARE_EXCLUSIVE))
		mode = SCARD_SHARE_SHARED;
	
#if VERSIONWIN
	CUTF16String reader;
	Param1_reader.copyUTF16String(&reader);
	LPTSTR lpszReaderName = (LPTSTR)reader.c_str();;
#else
	CUTF8String reader;
	Param1_reader.copyUTF8String(&reader);
	LPSTR lpszReaderName = (LPSTR)reader.c_str();
#endif
	
	SCARDCONTEXT hContext;
	LONG lResult;
	
	int is_card_present = 0;
 
	lResult = SCardEstablishContextEx(SCARD_SCOPE_USER, NULL, NULL, &hContext);
	if (lResult == SCARD_S_SUCCESS)
	{
		SCARD_READERSTATE readerState;
		readerState.szReader = lpszReaderName;
		readerState.dwCurrentState = SCARD_STATE_UNAWARE;
		
		Param_state.setIntValue(readerState.dwCurrentState);

		/* return immediately; check state */
		lResult = SCardGetStatusChange(hContext, 0, &readerState, 1);
		if (lResult == SCARD_S_SUCCESS)
		{
			if (readerState.dwEventState & SCARD_STATE_EMPTY)
			{
				/* SCARD_STATE_EMPTY */
				readerState.dwCurrentState = readerState.dwEventState;
				lResult = SCardGetStatusChange(hContext, timeout, &readerState, 1);
				if (readerState.dwEventState & SCARD_STATE_PRESENT)
				{
					/* SCARD_STATE_PRESENT */
					is_card_present = 1;
				}
			}else if (readerState.dwEventState & SCARD_STATE_UNAVAILABLE)
			{
				/* SCARD_STATE_UNAVAILABLE */
			}else if (readerState.dwEventState & SCARD_STATE_PRESENT)
			{
				/* SCARD_STATE_PRESENT */
				is_card_present = 1;
			}

			Param_state.setIntValue(readerState.dwEventState);
			
			if(is_card_present)
			{
				SCARDHANDLE hCard;
				DWORD dwActiveProtocol;
				DWORD dwProtocol;
				DWORD dwAtrSize;
				DWORD dwState;
				
				BYTE atr[256];

				C_BLOB temp;
				
				lResult = SCardConnect(hContext,
															 lpszReaderName,
															 mode,
															 protocols,
															 &hCard,
															 &dwActiveProtocol);
				switch (lResult)
				{
					case SCARD_W_REMOVED_CARD:
						/* SCARD_W_REMOVED_CARD */
						break;
					case SCARD_S_SUCCESS:
						lResult = SCardStatus(hCard, NULL, NULL, &dwState, &dwProtocol, atr, &dwAtrSize);
						if (lResult == SCARD_S_SUCCESS)
						{
							
							/*
							temp.setBytes((const uint8_t *)atr, dwAtrSize);
							temp.toHexText(&Param_atr);
							*/

							/* TODO: reading sys always fails...need investigation */
							/*
							 uint16_t scancode = SYSTEMCODE_ANY;
							 BYTE scancode_high = (scancode >> 8) & 0xFF;
							 BYTE scancode_low = (scancode)& 0xFF;
							BYTE pbSendBuffer_GetSys[10] = {
								APDU_CLA_GENERIC,
								APDU_INS_DATA_EXCHANGE,
								APDU_P1_THRU,
								APDU_P2_TIMEOUT_50MS,
								EXCHANGE_POLLING_PACKET_SIZE,
								EXCHANGE_POLLING,
								scancode_high,
								scancode_low,
								POLLING_REQUEST_SYSTEM_CODE,
								POLLING_TIMESLOT_16
							};
							 */
							
							BYTE pbSendBuffer_GetIDm[5] = {
								APDU_CLA_GENERIC,
								APDU_INS_GET_DATA,
								APDU_P1_GET_UID,
								APDU_P2_NONE,
								APDU_LE_MAX_LENGTH
							};
							
							BYTE pbSendBuffer_GetPMm[5] = {
								APDU_CLA_GENERIC,
								APDU_INS_GET_DATA,
								APDU_P1_GET_PMm,
								APDU_P2_NONE,
								APDU_LE_MAX_LENGTH
							};

							BYTE pbSendBuffer_GetCID[5] = {
								APDU_CLA_GENERIC,
								APDU_INS_GET_DATA,
								APDU_P1_GET_CARD_ID,
								APDU_P2_NONE,
								APDU_LE_MAX_LENGTH
							};

							BYTE pbSendBuffer_GetName[5] = {
								APDU_CLA_GENERIC,
								APDU_INS_GET_DATA,
								APDU_P1_GET_CARD_NAME,
								APDU_P2_NONE,
								APDU_LE_MAX_LENGTH
							};
							
							BYTE pbSendBuffer_GetType[5] = {
								APDU_CLA_GENERIC,
								APDU_INS_GET_DATA,
								APDU_P1_GET_CARD_TYPE,
								APDU_P2_NONE,
								APDU_LE_MAX_LENGTH
							};

							BYTE pbSendBuffer_GetTypeName[5] = {
								APDU_CLA_GENERIC,
								APDU_INS_GET_DATA,
								APDU_P1_GET_CARD_TYPE_NAME,
								APDU_P2_NONE,
								APDU_LE_MAX_LENGTH
							};
							
							BYTE pbRecvBuffer[256];
							DWORD pcbRecvLength = 256;

							/*
							lResult = getData(hCard,
								SCARD_PCI_T1,
								pbSendBuffer_GetSys,
								sizeof(pbSendBuffer_GetSys),
								NULL,
								pbRecvBuffer,
								&pcbRecvLength,
								Param_SystemCode,
								getDataMode_Sys);
								*/

							lResult = getData(hCard,
								SCARD_PCI_T1,
								pbSendBuffer_GetIDm,
								sizeof(pbSendBuffer_GetIDm),
								NULL,
								pbRecvBuffer,
								&pcbRecvLength,
								Param_IDm,
								getDataMode_ID);
							
							getData(hCard,
								SCARD_PCI_T1,
								pbSendBuffer_GetPMm,
								sizeof(pbSendBuffer_GetPMm),
								NULL,
								pbRecvBuffer,
								&pcbRecvLength,
								Param_PMm,
								getDataMode_ID);

							getData(hCard,
								SCARD_PCI_T1,
								pbSendBuffer_GetType,
								sizeof(pbSendBuffer_GetType),
								NULL,
								pbRecvBuffer,
								&pcbRecvLength,
								Param_Type,
								getDataMode_Type);
							
							getData(hCard,
											SCARD_PCI_T1,
											pbSendBuffer_GetCID,
											sizeof(pbSendBuffer_GetCID),
											NULL,
											pbRecvBuffer,
											&pcbRecvLength,
											Param_CID,
											getDataMode_Type);
							
							getData(hCard,
											SCARD_PCI_T1,
											pbSendBuffer_GetName,
											sizeof(pbSendBuffer_GetName),
											NULL,
											pbRecvBuffer,
											&pcbRecvLength,
											Param_Name,
											getDataMode_Name);

							getData(hCard,
											SCARD_PCI_T1,
											pbSendBuffer_GetTypeName,
											sizeof(pbSendBuffer_GetTypeName),
											NULL,
											pbRecvBuffer,
											&pcbRecvLength,
											Param_TypeName,
											getDataMode_Name);

						}/* SCardStatus */
						SCardDisconnect(hCard, SCARD_LEAVE_CARD);
						break;
					default:
						break;
				}/* SCardConnect */
			}
		}/* SCardGetStatusChange */
		SCardReleaseContext(hContext);
	}/* SCardEstablishContext */
#ifndef NOJSON	
	JSONNODE *json = json_new(JSON_NODE);
	json_set_s(json, L"IDm", Param_IDm);
	json_set_s(json, L"PMm", Param_PMm);
	json_set_i(json, L"state", Param_state);
	json_set_s(json, L"type", Param_Type);
	json_set_s(json, L"card", Param_CID);
	json_set_s(json, L"typeName", Param_TypeName);
	json_set_s(json, L"name", Param_Name);
	
	/*json_set_s(json, L"ATR", Param_atr);*/
	/*json_set_s(json, L"system", Param_SystemCode);*/
	
	json_stringify(json, returnValue);
	json_delete(json);
#endif
	returnValue.setReturn(pResult);
	
	Param5_error.setIntValue(lResult);
	Param5_error.toParamAtIndex(pParams, 5);
}

void SCARD_READER_LIST(sLONG_PTR *pResult, PackagePtr pParams)
{
	ARRAY_TEXT Param1_readers;
	ARRAY_TEXT Param2_cards;

	SCARDCONTEXT hContext;
	LONG lResult;
		
	LPTSTR lpszReaderName = NULL;
	LPTSTR pReader;
#if VERSIONWIN
	LPTSTR lpszCardName = NULL;
	LPTSTR pCard;
#endif
	
	lResult = SCardEstablishContextEx(SCARD_SCOPE_USER, NULL, NULL, &hContext);
	if (lResult == SCARD_S_SUCCESS)
	{
#if USE_FRAMEWORK
		/* SCARD_AUTOALLOCATE not supported in framework */
		uint32_t pcchReaders = 4096;
		char mszReaders[4096];
		lResult = SCardListReaders(hContext, SCARD_ALL_READERS, (LPTSTR)&mszReaders, &pcchReaders);
		lpszReaderName = mszReaders;
#else
		DWORD dwAutoAllocate = SCARD_AUTOALLOCATE;
		lResult = SCardListReaders(hContext, SCARD_ALL_READERS, (LPTSTR)&lpszReaderName, &dwAutoAllocate);
#endif
		switch (lResult)
		{
			case SCARD_E_NO_READERS_AVAILABLE:
				break;
			case SCARD_E_READER_UNAVAILABLE:
				break;
			case SCARD_S_SUCCESS:
				Param1_readers.setSize(1);
				pReader = lpszReaderName;
				while ('\0' != *pReader)
				{
#if VERSIONWIN
					CUTF16String reader = (PA_Unichar *)pReader;
					Param1_readers.appendUTF16String(&reader);
					pReader = pReader + wcslen((wchar_t *)pReader) + 1;
#else
					CUTF8String reader = (uint8_t *)pReader;
					Param1_readers.appendUTF8String(&reader);
					pReader = pReader + strlen((char *)pReader) + 1;
#endif
				}
#if USE_FRAMEWORK
#else
				SCardFreeMemory(hContext, lpszReaderName);
#endif				
				break;
			default:
				break;
		}/* SCardListReaders */
#if VERSIONWIN
		lResult = SCardListCards(NULL, NULL, NULL, NULL, (LPTSTR)&lpszCardName, &dwAutoAllocate);
		switch (lResult)
		{
			case SCARD_S_SUCCESS:
				Param2_cards.setSize(1);
				pCard = lpszCardName;
				while ('\0' != *pCard)
				{
					CUTF16String card = (PA_Unichar *)pCard;
					Param2_cards.appendUTF16String(&card);
					pCard = pCard + wcslen((wchar_t *)pCard) + 1;
				}
				SCardFreeMemory(hContext, lpszCardName);
					break;
				default:
					break;
				}/* SCardListCards */
#endif
		SCardReleaseContext(hContext);
	}/* SCardEstablishContext */
	
	Param1_readers.toParamAtIndex(pParams, 1);
	Param2_cards.toParamAtIndex(pParams, 2);
}
