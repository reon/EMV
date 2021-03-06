#include "ApplSelector.h"
#include "../Runtime/stdafx.h"

ApplSelector::ApplSelector()
{
	pAM = 0;
	btPSE = 0;
	PSE_len = 0;
	m_TransactionToken = 0;
	Terminal_Select = false;
	Terminal_Confirm = false;
	LanguageID = DEFAULT_LANG;
	pseIssuerCodeIndx = 0;
}

ApplSelector::~ApplSelector(void)
{
	closeServices();
	pAM = 0;
	resetPSE();
	ResetList(CandList);
	ResetAIDList();
}

long ApplSelector::getTransactionToken()
{
	return m_TransactionToken;
}

// Open all services
int ApplSelector::InitServices (AccessManager *am)
{
	int res;
	pAM = am;

	if (!pAM)
		return ERR_AM_NOT_INITIALIZED;;

	// Open SCR service
	if ((res = pAM->open(&SCR)) != SUCCESS)
	{
		pAM->close (UI);
		return res;
	}

	// Open CnfgControlInterface service
	if ((res = pAM->open(&CNFG)) != SUCCESS)
	{
		pAM->close (UI);
		pAM->close (SCR);
		return res;
	}
	return SUCCESS;
}

// Initializes transaction
int ApplSelector::InitializeTransaction(long &TransactionToken)
{
	int res;
	if (!SCR.initialized ())
	{
		TransactionToken = 0;
		if ((res = SCR.EstablishConnection ()) != SUCCESS)
		{
			m_TransactionToken = 0;
			TransactionToken = 0;
			return res;
		}
	}
	if (TransactionToken == 0)
	{
		res = SCR.BeginTransaction (TransactionToken);
	}
	else if (SCR.IsTransactionAlive(TransactionToken))
	{
		m_TransactionToken = TransactionToken;
		return SUCCESS;
	}
	else
		res = ERR_TRANSACTION_IN_PROGRESS;

	m_TransactionToken = 0;
	return res;
}

int ApplSelector::ApplicationSelection (long &TransactionToken,  
										tlv_parser *tlv_Appl)
{
	int res;
	bool LocalSession = false;
	CancelTransaction(); // cancel transaction
	uiEvent.releaseEvent(); // reset UI operation event

	if (!UI.opened ())
	{
		// Open UI service and start the application window
		if ((res = pAM->open (&UI)) != SUCCESS)
			return res;
		if ((res = UI.addOperationEvent (&uiEvent)) != SUCCESS)
		{
			pAM->close (UI);
			return res;
		}
		LocalSession = true;
	}

	// If a list of application supported by a terminal is empty, then do nothing
	if (ApplList.empty())
	{
		UI.writeStatus ("Terminal doesn't support any application",false);
		CancelTransaction();
		if (LocalSession)
			pAM->close (UI);
		return TERMINAL_APPL_LIST_IS_EMPTY;
	}
	
	
	// Begin a transaction
	if ((res = InitializeTransaction(TransactionToken)) != SUCCESS)
	{
		if (LocalSession)
			pAM->close (UI);
		return res;
	}
	

	ResetList(CandList);
	if (IsPSESupported())
		res = PaymentDirectorySelection();
	else
		res = ListOfAID();

	if (res == SUCCESS)
	{
		if (CandList.size () == 0)
		{
			UI.writeStatus ("Candidate List is Empty -- No matching Application is found");
			res = CANDIDATE_LIST_IS_EMPTY;
		}
		else
		{
			res = FinalSelection(tlv_Appl);
			if (res != SUCCESS)
			{
				UI.writeStatus("FAILED to select an application");
				// Stop the session
			}
		}
	}
	else
	{
		UI.writeStatus ("FAILED to build a list of mutually supported applications");	
	}

	if (res != SUCCESS)
	{
		ResetList(CandList);
		CancelTransaction();
	}

	TransactionToken = this->m_TransactionToken;
	if (LocalSession)
		pAM->close (UI);
	return res;
}

void ApplSelector::CancelTransaction()
{
	if (m_TransactionToken)
	{
		SCR.EndTransaction (m_TransactionToken);
		m_TransactionToken = 0;
	}
}

void ApplSelector::closeServices()
{
	pAM->close (UI);
	pAM->close (SCR);
	pAM->close (CNFG);
}

// Builds a list of applications supported by a terminal and initializes the
// terminal capability flags
// Returns SUCCESS if operation succeeds
int ApplSelector::BuildTerminalApplList ()
{
	int res;
	// Clear the content of a list of Application AIDs
	ResetAIDList();

	CnfgOperationEventImpl cnfgOpEvent;
	res = CNFG.addOperationEvent (&cnfgOpEvent);
	if (res != SUCCESS)
	{
		// Cannot add Operation Event
		return res;
	}

	// Get names of keys to the applications supported by a terminal
	res = CNFG.enumKeys (CNFG_POSAPPL, "ApplicationAID", NULL);
	if (res != SUCCESS)
	{
		// Failed to build an Application List
		return res;
	}
	char **ppListOfAppl;
	// Retreive the array of names ffrom the operation event object
	cnfgOpEvent.getStringArray (&ppListOfAppl);
	int len = cnfgOpEvent.getLength ();
	
	
	// Finding Application Info per each application
	CNFG.removeEvent ();
	CnfgOperationEventImpl opEv;
	CNFG.addOperationEvent (&opEv);
	
	byte *xcpAID;
	xcpAID = new byte [1];
	xcpAID[0] = 01;
	long xasi = 1;
	
	APPL_INFO *xpApplInfo = new APPL_INFO;
	xpApplInfo->AID = xcpAID;
	xpApplInfo->aid_len = 1;
	xpApplInfo->ASI = (byte) xasi;

	ApplList.push_back (xpApplInfo);
	
	
	
	for (int i = 0; i < len; i++)
	{
		char *path_to_aid = new char [strlen(ppListOfAppl[i]) + 22];
		strcpy (path_to_aid, ppListOfAppl[i]);
		/*
		strcat(path_to_aid, "\\");
		strcat(path_to_aid, "ApplicationInfo\\9F06");
		opEv.resetEvent(true);
		*/
		res = CNFG.getValue (CNFG_POSAPPL, path_to_aid);
		delete [] path_to_aid;
		if (res != SUCCESS)
			continue;

		byte *btAID;
		byte *cpAID;
		opEv.getByteString (&btAID);
		int size = opEv.getLength ();
		
		if (size < 20) // AID cannot be less then 5 bytes; go to the next AID
			continue;

		cpAID = new byte [size];
		if (!cpAID)
		{
			// Memory allocation error -- exit the operation
			ResetAIDList();
			return ERR_MEMORY_ALLOC;
		}
		byte buf[7];
		byte *pbuf = buf;
		int buflen = 7;
		res = AsciiStrWithSpace2HexByte((const char*)btAID, size, pbuf, &buflen);
	
		memcpy (cpAID, buf, buflen);


		// Read the value of ASI (Application Select Indicator)
		opEv.resetEvent (true);
		long asi;
		char *path_to_asi = new char [strlen(ppListOfAppl[i]) + 26];
		strcpy (path_to_asi, ppListOfAppl[i]);
		path_to_asi[11]='A';
		path_to_asi[12]='S';
		path_to_asi[13]='I';
		res = CNFG.getValue (CNFG_POSAPPL, path_to_asi);
		delete [] path_to_asi;
		if (res != SUCCESS)
		{
			if (opEv.getError () != ERR_REG_VALUE_READ_FAILED)
			{
				delete [] cpAID;
				continue;
			}
			else
				asi = 0;
		}
		else
		{
			byte *val = 0;
			opEv.getByteString (&val);
			int buflen2 = 1;
			res = AsciiStrWithSpace2HexByte((const char*)val, 2, pbuf, &buflen2);
			asi = buf[0] & 0x000000ff;
		}

		APPL_INFO *pApplInfo = new APPL_INFO;
		if (!pApplInfo)
		{
			// Memory Allocation Error -- exit application
			delete [] cpAID;
			ResetAIDList();
			return ERR_MEMORY_ALLOC;
		}
		pApplInfo->AID = cpAID;
		pApplInfo->aid_len = buflen;
		pApplInfo->ASI = (byte) asi;

		ApplList.push_back (pApplInfo);
	}

	// GET UI Flags
	Terminal_Select = false;
	Terminal_Confirm = false;

	opEv.resetEvent(true);
	// Get UI Flags 
	res = CNFG.getValue(CNFG_TERMINAL, "Data", "50000002");
	if (res == SUCCESS)
	{
		byte *val;
		res = opEv.getByteString (&val);
		if (res == SUCCESS)
		{
			if (check_bit(*val, 0x2))
				Terminal_Select = true;
			if (check_bit(*val, 0x1))
				Terminal_Confirm = true;
		}
	}

	// Initialize AdditionalTerminalCapabilities array
	memset (AdditionalTerminalCapabilities, 0, 5);
	opEv.resetEvent(true);
	res = CNFG.getValue(CNFG_TERMINAL, "Data", "9F40");
	if (res == SUCCESS)
	{
		byte *val;
		int len;
		res = opEv.getByteString (&val);
		if (res == SUCCESS)
		{
			len = opEv.getLength ();
			if (len == 5)
				memcpy (AdditionalTerminalCapabilities, val, 5);
		}
	}
	opEv.resetEvent(true);
	
	// Get Language ID
	res = CNFG.getValue(CNFG_TERMINAL, "Data", "50000001");
	if (res == SUCCESS)
	{
		byte *val;
		res = opEv.getByteString (&val);
		if (res == SUCCESS)
		{
			LanguageID = 0x000000ff & (*val);
		}
	}

	CNFG.removeEvent ();
	return SUCCESS;
}

// Finds a name of the PSE directory
int ApplSelector::GetPSE()
{
	int res;
	resetPSE();
	if (btPSE)
	{
		delete [] btPSE;
		btPSE = NULL;
	}
	
	CnfgOperationEventImpl cnfgOpEvent;
	
	res = CNFG.addOperationEvent (&cnfgOpEvent);
	if (res != SUCCESS)
	{
		//Cannot add Operation Event
		return res;
	}

	cnfgOpEvent.resetEvent (true);

	res = CNFG.getValue (CNFG_TERMINAL, "PSE_BIN", "DirectorySelection");
	if (res == SUCCESS)
	{
		int len = cnfgOpEvent.getLength ();
		if (len <= 0)
		{
			// Invalid format of PSE key stored in the registry
			return ERR_INVALID_PSE_FORMAT;
		}

		byte *pTmp = new byte [len];
		if (!pTmp)
			return ERR_MEMORY_ALLOC;
		byte *pTmp2;
		cnfgOpEvent.getByteString (&pTmp2);
		memcpy(pTmp, pTmp2, len);
		btPSE = pTmp;
		PSE_len = len;
	}

	CNFG.removeEvent ();
	return SUCCESS;
}

// Implements a List-of-AIDs application selection method
int ApplSelector::ListOfAID()
{
	PSEMethod = false; // List-of-AID method is performed
	int res;
	scr_command command (&SCR);

	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	OutputMsg  ("Using List Of Applications method to select the application");

	// Per each AID in a list of application supported by a terminal
	// issue a SELECT command to the card to selct the application with this AID.
	list<APPL_INFO*>::iterator it;
	
	// 1. Get First AID from the List of AID's supported by a terminal
	it = ApplList.begin ();
	
	bool FirstAndOnly = true;
	R_APDU rapdu;

	while (it != ApplList.end() && m_TransactionToken != 0)
	{
		APPL_INFO *applInfo = *it;

		// Create a SELECT command
		if ((res = command.setSelect((*it)->AID, 
			(*it)->aid_len, true, FirstAndOnly)) != SUCCESS)
			break;
				
		// Execute a select command
		if((res = command.run (&rapdu, m_TransactionToken)) == SUCCESS)
		{   
			// Command successfully executed,
			// Take an appropriate action based on the status code
			if (rapdu.getSW1() == 0x6A && rapdu.getSW2 () == 0x81)
			{   
				// 3. Card is blocked or function is not supported by ICC
				OutputMsg ("Card is blocked or the command is not supported");
				res = ERR_ICC_FUNCTION_NOT_SUPPORTED;
				break; // Exit While loop
			}
			else if ((rapdu.getSW1() == 0x90 && rapdu.getSW2 () == 0x00) ||
				    (rapdu.getSW1() == 0x62 && rapdu.getSW2 () == 0x83))
			{
				// File is found!
				byte *data = rapdu.copyData();
				if (!data)
				{
					res = ERR_INVALID_ICC_DATA;
					break;
				}

				tlv_parser *tlv_resp = new tlv_parser;
				res = tlv_resp->parse (data, rapdu.getDataLen ());
				if (res != SUCCESS)
				{
					// Failed to parse the response header from TLV format
					delete [] data;
					delete tlv_resp;
					break; // Exit while loop
				}
				
				tlv_parser *dfName = 0;
				if ((res = CheckFCITemplateAdf(tlv_resp, &dfName)) != SUCCESS)
				{
					// Failed to extract DF Name from the response header
					delete [] data;
					delete tlv_resp;
					break;
				}
				else
				{
					int same_res;
					same_res = CompareAID(&rapdu,
										  dfName->GetRoot()->GetValue(), 
										  dfName->GetRoot()->GetLengthVal(),
										  (*it)->AID, 
										  (*it)->aid_len, 
										  (*it)->ASI);
					if (same_res >= 0)
					{
						// Add to a candidate list
						CandList.push_back (tlv_resp);
					}
					else
					{
						// Do not add to the list; release resources
						delete [] data;
						delete tlv_resp;
					}
					if (same_res == 0 || same_res == -1)
					{
						// Go to the next AID in a list
						it++;
						FirstAndOnly = true;
					}
					else //if (same_res == 1 || same_res == -2)
					{
						// Do not go to the next AID, and try to find the next application
						// with the same AID
						FirstAndOnly = false;
					}					
				}
			}
			else  // Error Code (File Not found)
			{
				// Some other error code is returned; proceed with the next AID
				it++;
				FirstAndOnly = true;
			}
		}
		else // Send Command
		{
			// Send Command failed; terminate the transaction
			break;  // Exit while loop
		}
	} // While Loop

	if (res != SUCCESS)
	{
		CancelTransaction();
		ResetList(CandList);
	}
	return res;
}

// Implements a Payment-System-Directory application selection method
int ApplSelector::PaymentDirectorySelection()
{
	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	PSEMethod = true; // PSE method is performed
	// Initialize and begin a transaction
	int res;
	C_APDU capdu;
	R_APDU rapdu;
	scr_command command(&SCR);

	// Select PSE directory
	// Create SELECT command apdu
	if ((res = command.setSelect(btPSE, PSE_len, true, true))
		!= SUCCESS)
		return res;

	// Execute SELECT command
	if ((res = command.run(&rapdu, m_TransactionToken)) != SUCCESS)
		return res;

	// Check the status code returned by a SELECT command
	if (rapdu.getSW1() == 0x6A && rapdu.getSW2 () == 0x81)
	{   
		// Card is blocked or function is not supported by ICC
		OutputMsg ("Card is blocked or the command is not supported");
		return ERR_ICC_FUNCTION_NOT_SUPPORTED;
	}
	else if (rapdu.getSW1() == 0x6A && rapdu.getSW2 () == 0x82)
	{
		// There is no PSE in the ICC -- use select list application method
		OutputMsg ("PSE not found in the ICC -- using select list application method");
		return ListOfAID();
	}
	else if (rapdu.getSW1() == 0x62 && rapdu.getSW2 () == 0x83)
	{
		// PSE is blocked -- use the list of applications method
		OutputMsg ("PSE is blocked -- using the list of applications method");
		return ListOfAID();
	}
	else if (rapdu.getSW1() != 0x90 || rapdu.getSW2 () != 0x00)
	{
		// Some other error condition -- use the list of applications method
		OutputMsg ("Some other error condition -- using the list of applications method");
		return ListOfAID();
	}

	// SELECT command returned 0x9000 status code -- continue with PSE method
	OutputMsg ("Using the Payment System Directories method to select the application");

	// Parse the data in response apdu
	byte *data = rapdu.copyData();
	tlv_parser tlv_pse;
	res = tlv_pse.parse(data, rapdu.getDataLen ());
	if (res != SUCCESS)
	{
		// Failed to parse the data returned by SELECT (PSE) command
		delete [] data;
		return res;
	}

	byte sfi;
	// Reset Issuer Code Table Index for PSE
	pseIssuerCodeIndx = 0;
	if ((res = CheckFCITemplateDir(true, &tlv_pse, &sfi)) != SUCCESS)
	{
		delete [] data;
		return res;
	}
	res = SelectDirectories (sfi);

	// Release resources
	delete [] data;

	if (res == SUCCESS && CandList.size () == 0)
		return ListOfAID();
	
	return res;
}

// Implements a final selection of the application
int ApplSelector::FinalSelection(tlv_parser *tlv_Appl)
{
	if (CandList.empty ())
		return NO_COMMON_APPLICATIONS;
	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	int res;

	bool LocalSession = false;
	uiEvent.releaseEvent(); // reset UI operation event

	if (!UI.opened ())
	{
		// Open UI service and start the application window
		if ((res = pAM->open (&UI)) != SUCCESS)
			return res;
		if ((res = UI.addOperationEvent (&uiEvent)) != SUCCESS)
		{
			pAM->close (UI);
			return res;
		}
		LocalSession = true;
	}

	SortCandidateList();
	UI.resetOption (OPTIONLIST_APPLICATIONS);

	res = UI.setLanguage (EMV_LANG_LATIN1);
		
	if (CandList.size () == 1)
		res = SelectAutomatic(tlv_Appl);
	else if (Terminal_Select)
		res = SelectFromList(tlv_Appl);
	else 
		res = SelectAutomatic(tlv_Appl);

	if (res != SUCCESS)
	{
		SetPrompt(MSG_ID__NOT_ACCEPTED, "Not Accepted");
	    
		// Simulate a delay of 3 seconds to allow user to see the message
		int evID []= {BTN_ENTER, BTN_CANCEL};
		int evID2;
		UI.waitForEvent (evID, 2, &evID2, 3000);
	}
	if (LocalSession)
	{
		UI.close ();
		uiEvent.releaseEvent ();
	}
	return res;
}

// Sorts a candidate list in a priority order where priority 1 is 
//  the highest priority
void ApplSelector::SortCandidateList()
{
	PARSER_LIST::iterator it1;
	PARSER_LIST::iterator it_right;
	PARSER_LIST::iterator it_left;
	tlv_parser *parser_left;
	tlv_parser *parser_right;
	

	for (it1 = CandList.begin ();it1 != CandList.end(); it1++)
	{
		it_left = it1;
		it_right = it1;
		it_right++;
		for (;it_right != CandList.end(); it_right++)
		{
			parser_left = (*it_left)->Find (0x87, true);
			parser_right = (*it_right)->Find (0x87, true);
			if (Compare(parser_left, parser_right) > 0)
				it_left = it_right; // Do swapping
		}
		parser_left = *it1;
		*it1 = *it_left;
		*it_left = parser_left;
	}
}

// Prompts a user to Confirm currently selected application
int ApplSelector::ConfirmApplSelection (tlv_parser *parser, bool *bSelected)
{
	int res;
	uiEvent.resetEvent (true);
	bool ConfirmFlag = false;
	byte CodeIndx;
	char *preferedName = 0;

	if (PSEMethod && pseIssuerCodeIndx != 0)
	{
		// PSE Directory selection method has been used.
		// Use Issuer Code Table index specified in the FCI of the PSE.
		// this value is initialized in the CheckFCITemplateDir function
		CodeIndx = pseIssuerCodeIndx;
		res = SUCCESS;
	}
	else
	{
		// Direct directory method has been used to create a candidate list.
		// Use Issuer Code Table Index stored in the FCI of an ADF.
		res = GetIssuerCodeIndx(parser, &CodeIndx);
		if (res == ERR_INVALID_ICC_DATA)
		{
			*bSelected = false;
			return res;
		}
	}
	if (res == SUCCESS)
	{
		if (Language::IsCodeTableSupportedByTerminal(CodeIndx,
						AdditionalTerminalCapabilities))
		{
			preferedName = GetPreferedName(parser);
		}
	}
	if (preferedName)
	{
		// Prefered Name is found, use it for conformation message
		char *temp = new char [strlen(preferedName) + 3];
		if (!temp)
		{
			delete [] preferedName;
			*bSelected = false;
			return ERR_MEMORY_ALLOC;
		}
		strcpy (temp, preferedName);
		strcat(temp, " ?");
		int LangID = CodeIndx;
		delete [] preferedName;
		
		printf ("Timeout is 5000\n");
		res = UI.getResponse (temp, USER_RESPONSE_TO, LangID);
		
		delete[] temp;
		if (res == SUCCESS)
		{
			int btn;
			uiEvent.getButton (&btn);
			if (btn == BTN_ENTER)
				ConfirmFlag = true;
			else
				ConfirmFlag = false;
		}
		else
			ConfirmFlag = false;
	}
	else
	{   // Prefered name is not found, try to find Application label
		char *ApplLabel = GetApplLabel(parser);
		if (!ApplLabel)
		{
			// If Application label is missing, use AID instead
			ApplLabel = GetAIDasStr(parser);
			if (!ApplLabel)
			{
				ApplLabel = new char [12];
				if (!ApplLabel)
					return ERR_MEMORY_ALLOC;
				strcpy(ApplLabel, "NO APPL ID");
			}
		}
		
		char *temp = new char [strlen(ApplLabel) + 3];
		if (!temp)
		{
			delete [] ApplLabel;
			*bSelected = false;
			return ERR_MEMORY_ALLOC;
		}
		strcpy (temp, ApplLabel);
		strcat(temp, " ?");
		delete [] ApplLabel;
		res = UI.getResponse (temp, USER_RESPONSE_TO);
		delete[] temp;
		if (res == SUCCESS)
		{
			int btn;
			uiEvent.getButton (&btn);
			if (btn == BTN_ENTER)
				ConfirmFlag = true;
			else
				ConfirmFlag = false;
		}
		else
			ConfirmFlag = false;
		
	}
	uiEvent.resetEvent (true);
	*bSelected = ConfirmFlag;
	return SUCCESS;
}

// Automatically selects an application from the candidate list 
	//  (used when a terminal doesn't support selction)
int ApplSelector::SelectAutomatic(tlv_parser *tlv_Appl)
{
	int res = EVT_ERROR;
	bool ApplConfirm;
	int ApplSelected = APPLICATION_NOT_SELECTED;
	tlv_parser *parser;
	bool DoSelect;

	while (!CandList.empty() && m_TransactionToken != 0)
	{
		// Get the Application with the highest priority from the list
		parser = CandList.front ();
		// Remove the current item from the list
		CandList.pop_front ();
		ApplConfirm = IsApplReqConfirm(parser);
		
		// Choose from 5 possible cases whether or not to confirm the application
		// selection. 
		if (ApplConfirm && !Terminal_Confirm) // 1st case
		{	// Application requests confirmation -- Terminal doesn't support confirmation
			// Remove the application from the list of candidates
			DoSelect = false;
		}
		else // The rest 4 cases
		{
			if (ApplConfirm) // 2)Always confirm if Application requires confirmation
			{
				// This is a (ApplConfirm && Terminal_Confirm) condition
				res = ConfirmApplSelection(parser, &DoSelect); 
				if (res != SUCCESS)
				{
					// Release memory for the current application
					delete [] parser->GetTlvDataObject();
					delete parser;
					
					ApplSelected = res;
					break;
				}
				//DoSelect = ConfirmApplSelection (parser);
			}
			else if (CandList.size() == 0) 
			{
				// 3)This is  a (!ApplConfirm && (Terminal_Confirm || !Terminal_Confirm) && CandList.size() == 0) condition
				// Since Appl doesn't require confirmation and there are no more applications
				// in the candidate list except current application, then do not confirm
				DoSelect = true;
			}
			else if (Terminal_Confirm)
			{
				// 4)This is (!ApplConfirm && Terminal_Confirm) condition
				// Application doesn't require confirmation, however, since
				// the terminal supports confirmation, therefore we Do confirmation
				res = ConfirmApplSelection(parser, &DoSelect); 
				if (res != SUCCESS)
				{
					ApplSelected = res;
					// Release memory for the current application
					delete [] parser->GetTlvDataObject();
					delete parser;
					break;
				}
				//DoSelect = ConfirmApplSelection (parser);
			}
			else
			{
				// 5)This is a (!ApplConfirm && !Terminal_Confirm) condition
				DoSelect = true;
			}
		}
		if (DoSelect && m_TransactionToken != 0)
		{
			res = SelectApplication (parser, tlv_Appl);
			
			// Release memory for the current application
			delete [] parser->GetTlvDataObject();
			delete parser;
			
			if (res == SUCCESS)
			{
				// Application is successfully selected
				// Exit the While loop
				ApplSelected = res;
				break;
			}
			else
			{
				// There was an error selecting the application.
				if (CandList.empty())
				{
					ApplSelected = APPLICATION_NOT_SELECTED;
					break;
				}

				if (Terminal_Confirm)
				{
					// Display the 'Try Again' message only if terminal supports
					// confirmation. See EMV book 4, ch 7.3 3rd bullet
					const char *prompt = Language::getString (MSG_ID__TRY_AGAIN, DEFAULT_LANG);
					uiEvent.resetEvent (true);
					int btn_res;
					if (prompt)
						btn_res = UI.getResponse (prompt);
					else
						btn_res = UI.getResponse ("Try Again");
					if (btn_res == SUCCESS)
					{
						int btn;
						uiEvent.getButton (&btn);
						uiEvent.resetEvent (true);
						if (btn != BTN_ENTER)
						{
							ApplSelected = res;
							break;
						}
					}
					else
					{
						ApplSelected = res;
						break;
					}
				}
			}
		}
		else
		{		
			// A user didn't confirmed the application selection, or
			// Application cannot be confirmed.
			
			// Remove the application from the list
			delete [] parser->GetTlvDataObject();
			delete parser;
		}
	} // While-not-empty loop
	
	uiEvent.resetEvent (true);
	return ApplSelected;
}


// Presents a list of mutually supported applications to the user for selection
int ApplSelector::SelectFromList(tlv_parser *tlv_Appl)
{
	int res;
	tlv_parser *parser;
	size_t numOfCandidates = CandList.size ();
	int i;
	
	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	char **ApplArr = new char* [numOfCandidates];
	if (!ApplArr)
		return ERR_MEMORY_ALLOC;

	int *LangArr = new int [numOfCandidates];
	if (!LangArr)
	{
		delete [] ApplArr;
		return ERR_MEMORY_ALLOC;
	}

	// Build arrays of applications names and language ids
	PARSER_LIST::iterator it;
	for (it = CandList.begin (), i = 0;it != CandList.end(); it++)
	{
		parser = *it;
		
		byte CodeIndx;
		if (PSEMethod && pseIssuerCodeIndx != 0)
		{
			// PSE Directory selection method has been used to create a candidate list.
			// Use Issuer Code Table index specified in the FCI of the PSE.
			// this value is initialized in the CheckFCITemplateDir function
			CodeIndx = pseIssuerCodeIndx;
			res = SUCCESS;
		}
		else
		{
			res = GetIssuerCodeIndx(parser, &CodeIndx);
			if (res != SUCCESS && res != CODE_TABLE_INDX_NOT_FOUND)
				break;
		}

		char *preferedName = 0;
		if (res == SUCCESS)
		{
			if (!Language::IsValidCodeTable(CodeIndx))
			{
				res = ERR_INVALID_ICC_DATA;
				break;
			}
			if (Language::IsCodeTableSupportedByTerminal(CodeIndx,
							AdditionalTerminalCapabilities))
			{
				preferedName = GetPreferedName(parser);
				if (preferedName)
				{
					LangArr[i] = CodeIndx;
					ApplArr[i] = preferedName;
					i++;
				}
			}
		}
		if (!preferedName)
		{
			res = SUCCESS;
			char *ApplLabel = GetApplLabel(parser);
			if (!ApplLabel)
			{
				// If Application label is missing, use AID instead
				ApplLabel = GetAIDasStr(parser);
				if (!ApplLabel)
				{
					ApplLabel = new char [12];
					if (!ApplLabel)
						return ERR_MEMORY_ALLOC;
					strcpy(ApplLabel, "NO APPL ID");
				}
			}
			LangArr[i] = DEFAULT_LANG;
			ApplArr[i] = ApplLabel;
			i++;
		}
	} // End of FOR loop

	if (res != SUCCESS)
	{
		for (int j = 0; j < i; j++)
			delete [] ApplArr[j];
		delete [] LangArr;
		delete [] ApplArr;
		return res;
	}

	// Loop until either application is selected, or there is at least one element
	// in the array of applications names
	UI.setPrompt ("Select the application");
	res = APPLICATION_NOT_SELECTED;
	while (i > 0 && m_TransactionToken != 0)
	{
		// Add Application names or labels to the option list	
		uiEvent.resetEvent (true);
		res = UI.selectOption (OPTIONLIST_APPLICATIONS, NULL, ApplArr, 0, i, 
							   USER_RESPONSE_TO, LangArr);
		if (res == SUCCESS)
		{
			int selected;
			uiEvent.getButton(&selected);
			if (selected == BTN_ENTER)
			{
				// Enter button is clicked, find out the index of the selected application
				uiEvent.getIndex (&selected);
				if (selected < 0)
				{
					// None of the application in the list have been selected, 
					// try again
					SetPrompt (MSG_ID__TRY_AGAIN, "Try Again");
					continue;
				}

				// Find an element in the list at the <selected> position 
				it = CandList.begin ();
				for (int k = 0; k < selected; k++)
					it++;

				// Select the application chosen from the list
				res = SelectApplication (*it, tlv_Appl);
				if (res == SUCCESS)
				{
					// Application is successfully selected
					// Remove the current application from the list,
					RemoveFromList (it, CandList, ApplArr, LangArr, selected, i);
					// break WHILE loop
					i--;
					break;
				}
				else
				{
					// Error selecting the application --
					// Remove the current application and try again
					RemoveFromList (it, CandList, ApplArr, LangArr, selected, i);
					i--;
					// Display Try Again message
					SetPrompt (MSG_ID__TRY_AGAIN, "Try Again");
				}
			}
			else
			{
				// Some other button instead of Enter has been clicked,
				// Therefore we assume that the operation has been canceled 
				res = OPERATION_CANCELED_BY_USER;
				break;
			}
		}
		else
		{
			// SelectOption operation complited with the error
			// Exit the selection process
			res = uiEvent.getError ();
			break;
		}
	} // End Of While Loop
	
	for (int j = 0; j < i; j++)
		delete ApplArr[j];
	delete [] ApplArr;
	delete [] LangArr;
	// reset option List
	UI.resetOption (OPTIONLIST_APPLICATIONS);
	if (m_TransactionToken == 0)
		res = TRANSACTION_CANCELED;
	return res;
}


void ApplSelector::RemoveFromList (PARSER_LIST::iterator &it,
				PARSER_LIST &CandList, char **ApplArr, int *LangArr, 
				int curIndx, int maxIndx)
{
	delete ApplArr[curIndx];
	for (int j = curIndx + 1; j < maxIndx; j++)
	{
		ApplArr[j - 1] = ApplArr [j];
		LangArr[j - 1] = LangArr [j];
	}
	delete [] (*it)->GetTlvDataObject();
	delete *it;
	CandList.erase (it);
}

// Selects an application AID of which is stored inside parser.
int ApplSelector::SelectApplication (tlv_parser *parser, 
									 tlv_parser *tlv_Appl)
{
	 byte applName;
	 if (m_TransactionToken == 0)
		 return NO_TRANSACTION;

	 (PSEMethod)? applName = 0x4F: applName = 0x84;

	 tlv_parser *parser_aid = parser->Find (applName);
	 if (!parser_aid)
		 return AID_PARSE_ERROR;
	
	 return SelectApplication (m_TransactionToken,
							   parser_aid->GetRoot()->GetValue (),
							   parser_aid->GetRoot ()->GetLengthVal (),
							   tlv_Appl);
 }

int ApplSelector::SelectApplication (long TransToken,
									 byte *aid, int aid_len, 
									 tlv_parser *tlv_Appl)
{
	int res;
	scr_command command (&SCR);
	 
	if ((res = command.setSelect(aid, aid_len, true, true)) != SUCCESS)
	{
		return res;
	}
	R_APDU rapdu;
	if ((res = command.run(&rapdu, TransToken)) != SUCCESS)
	{
		return res;
	}

	if (rapdu.getSW1() != 0x90 || rapdu.getSW2 () != 0x00)
	{   
		//  Status code is NOT Success
		return APPLICATION_NOT_SELECTED;
	}

	byte *data = rapdu.copyData();
	if (!data)
		return ERR_INVALID_ICC_DATA;

	res = tlv_Appl->parse (data, rapdu.getDataLen ());
	if (res != SUCCESS)
	{
		delete [] data;
		return res;
	}
	
	if ((res = CheckFCITemplateAdf(tlv_Appl, 0)) != SUCCESS)
	{
		delete [] data;
		tlv_Appl->reset();
		return res;
	}

	return SUCCESS;
}

// releases a memory allocated by PSE
void ApplSelector::resetPSE()
{
	if (btPSE)
	{
		delete [] btPSE;
		btPSE = NULL;
	}
	PSE_len = 0;
}

// releases memory allocated to the list of type PARSER_LIST
void ApplSelector::ResetList(PARSER_LIST &parser_list)
{
	while (!parser_list.empty ())
	{
		delete [] parser_list.front ()->GetTlvDataObject();
		delete parser_list.front ();
		parser_list.pop_front ();
	}
}

// releases memory allocated for the List of terminal AIDs
void ApplSelector::ResetAIDList ()
{
	while (!ApplList.empty ())
	{
		delete [] ApplList.front()->AID;
		delete ApplList.front ();
		ApplList.pop_front ();
	}
}


// compare 2 tlv_parser objects to have the same value (used in comparing AIDs)
int ApplSelector::Compare (tlv_parser *val_left, tlv_parser *val_right)
{
	if (!val_left && !val_right)
		return 0;
	else if (!val_left && val_right)
		return 1;
	else if (val_left && !val_right)
		return -1;
	else
	{
		byte bt_left = val_left->GetRoot ()->GetValue ()[0];
		byte bt_right = val_right->GetRoot ()->GetValue ()[0];
		if ((0x0f & bt_left) < (0x0f & bt_right))
			return -2;
		else
			return 2;
	}
}

// checks if teh application requires confirmation
bool ApplSelector::IsApplReqConfirm(tlv_parser *parser)
{
	tlv_parser *priority = parser->Find (0x87, true);
	if (!priority)
		return false;

	if (check_bit(priority->GetRoot ()->GetValue ()[0], 0x80))
		return true;
	else
		return false;
}

// Reads all records from the file identified by SFI and stores those records
	// in a RecordStack
int ApplSelector::ReadRecords (byte SFI, 
							   scr_command &command, PARSER_LIST &RecordsStack)
{
	int res;
	byte record = 0;
	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	// For each record in the directory
	while (m_TransactionToken != 0)
	{
		record++;
		// set Read Record command
		if ((res = command.setReadRecord(record, SFI)) != SUCCESS)
			return res;
	
		R_APDU rapduRec;
		// Execute Read Record command
		if ((res = command.run(&rapduRec, m_TransactionToken)) != SUCCESS)
		{
			return res;
		}
		
		if (rapduRec.getSW1 () == 0x6a && rapduRec.getSW2 () == 0x83)
			break; // Exit While loop (No more records to read)
		
		if (rapduRec.getSW1 () != 0x90 || rapduRec.getSW2 () != 0x00)
		{
			// Invalid response to the ReadRecord command -- terminate transaction
			return ERR_CMD_INVALID_ICC_RESPONSE;
		}

		byte *rec_data = rapduRec.copyData ();
		if (!rec_data)
			return ERR_MEMORY_ALLOC;

		tlv_parser *tlv_record = new tlv_parser();
		if (!tlv_record)
		{
			delete [] rec_data;
			return ERR_MEMORY_ALLOC;
		}

		if ((res = tlv_record->parse(rec_data, rapduRec.getDataLen())) != SUCCESS)
		{
			// Failed to parse the data returned by SELECT (PSE) command
			delete [] rec_data;
			delete tlv_record;
			return res;
		}
		else
		{
			//Parsing succeeded
			RecordsStack.push_front (tlv_record);
		}
	}
	if (m_TransactionToken == 0)
		return TRANSACTION_CANCELED;

	return SUCCESS;
}

// Reads all records from the file identified by SFI and stores those records
	// in a RecordStack
int ApplSelector::ReadDirRecord (byte SFI, byte record, 
							     scr_command &command, 
								 tlv_parser **tlv_Dir)
{
	int res;
	*tlv_Dir = 0;
	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	// set Read Record command
	if ((res = command.setReadRecord(record, SFI)) != SUCCESS)
		return res;
	
	R_APDU rapduRec;
	// Execute Read Record command
	if ((res = command.run(&rapduRec, m_TransactionToken)) 
		!= SUCCESS)
	{
		return res;
	}
		
	if (rapduRec.getSW1 () == 0x6a && rapduRec.getSW2 () == 0x83)
		return SUCCESS; // No more records to read
		
	if (rapduRec.getSW1 () != 0x90 || rapduRec.getSW2 () != 0x00)
	{
		// Invalid response to the ReadRecord command -- 
		//   terminate transaction
		return ERR_CMD_INVALID_ICC_RESPONSE;
	}

	byte *rec_data = rapduRec.copyData ();
	if (!rec_data)
		return ERR_MEMORY_ALLOC;

	tlv_parser *tlv_record = new tlv_parser();
	if (!tlv_record)
	{
		delete [] rec_data;
		return ERR_MEMORY_ALLOC;
	}

	if ((res = tlv_record->parse(rec_data, rapduRec.getDataLen())) != SUCCESS)
	{
		// Failed to parse the data returned by SELECT (PSE) command
		delete [] rec_data;
		delete tlv_record;
		return res;
	}
	*tlv_Dir = tlv_record;
	return SUCCESS;
}

// Finds a prefered name if it exists in FCI represented by parser
char* ApplSelector::GetPreferedName(tlv_parser *parser)
{
	return extractString(parser, 0x9F12);
}


// Finds an application label if it exists in FCI represented by parser
char* ApplSelector::GetApplLabel(tlv_parser *parser)
{
	return extractString(parser, 0x50);
}

char* ApplSelector::GetAIDasStr(tlv_parser *parser)
{
	byte tag;
	if (PSEMethod)
		tag = 0x4f;
	else
		tag = 0x84;

	tlv_parser *aid = parser->Find (tag, true);
	if (!aid)
		return 0;

	return HexByte2AsciiStr(aid->GetRoot()->GetValue(), 
		aid->GetRoot ()->GetLengthVal());
}

// extracts an ASCII string from the element identified by Tag
char* ApplSelector::extractString (tlv_parser *parser, int Tag)
{
	tlv_parser *tlv_string = parser->Find (Tag, true);
	if (!tlv_string)
		return NULL;

	int len = tlv_string->GetRoot ()->GetLengthVal ();
	char *name = new char [len + 1];
	memcpy (name, tlv_string->GetRoot ()->GetValue (), len);
	name [len] = '\0';
	return name;
}

// Retreives the IssuerCodeTable index identified in FCI
int ApplSelector::GetIssuerCodeIndx(tlv_parser *parser, byte *CodeIndx)
{
	*CodeIndx = 0;
	tlv_parser *tlv_lang = parser->Find (0x9F11, true);
	if (tlv_lang)
	{
		int len = tlv_lang->GetRoot ()->GetLengthVal ();
		if (len != 1)
			return ERR_INVALID_ICC_DATA;

		*CodeIndx =  tlv_lang->GetRoot ()->GetValue()[0];
		if (!Language::IsValidCodeTable(*CodeIndx))
			return ERR_INVALID_ICC_DATA;
		else
			return SUCCESS;
	}
	else
		return CODE_TABLE_INDX_NOT_FOUND;
}

// Sets a prompt whether based on MsgID or a string Msg
void ApplSelector::SetPrompt(int MsgID, const char *Msg)
{
	// Display Try Again message
	const char *prompt = Language::getString (MsgID, DEFAULT_LANG);
	if (prompt)
		UI.setPrompt (prompt);
	else
		UI.setPrompt (Msg);
}

// Compares two AIDs stored as byte arrays
	// Returns:
	//  0 - AIDs are identical and application is not blocked
	// -1 - AIDs are identical and application is blocked or
	//		AIDs are different
	//  1 - AIDs are identical up to the length of the terminal AID.
	//		partial selection is allowed, 
	//		application is not blocked
	// -2 - same as 1, but application is blocked, or 
	//		partial selection is not allowed
int ApplSelector::CompareAID(R_APDU *rapdu, byte *dfName, int dfName_len, 
				byte *aid_term, int aid_len, int asi)
{
	int same_res;
	if ((same_res = IsSameAID(dfName, dfName_len, aid_term, aid_len)) == 0)
	{
		// DF NAme and AID are identical; 
		if (!(rapdu->getSW1() == 0x62 && rapdu->getSW2 () == 0x83))
		{
			// Application is NOT blocked;
			// Add FCI to a candidate list -- go to the next AID in a list
			return 0; 
		}
		else
		{
			// Application is blocked;
			// Do not add FCI to a candidate list -- go to the next AID in a list
			return -1;
		}
	}
	else if (same_res == 1)
	{
		// AID's are identical up to the length of 
		// the Terminal AID (aid_len)
		if (asi == 0x01)
		{
			// Partial selection is allowed;
			if (!(rapdu->getSW1() == 0x62 && rapdu->getSW2 () == 0x83))
			{
				// Application is not blocked;
				// Add FCI to a candidate list -- select next appl with the same AID
				return 1;
			}
			else
			{
				// Allication is blocked; 
				// Do not add it to the candidate list -- select next appl with the same AID
				return -2;
			}
		}
		else
		{   // Partial Selection is not allowed; 
			// Do not add it to the list -- select next appl with the same AID
			return -2;
			
		}
	}
	else
	{
		// AID's are different; 
		// Do not add AID to the candidate list -- go to the next AID in a list
		return -1;
	}
}

// Finds AID in a list of application supported bt a terminal 
bool ApplSelector::FindAID (byte *aid, int aid_len, APPL_LIST *ApplList)
{
	list<APPL_INFO*>::iterator it;
	int res;
	printf("Looking for application: ");
	for(int i = 0; i < aid_len ; i++){
		printf("%02X",aid[i]);
	}
	printf("\n");
	for (it = ApplList->begin (); it != ApplList->end (); it++)
	{
		res = IsSameAID(aid, aid_len, (*it)->AID, (*it)->aid_len);
		if ((*it)->ASI == 1 && res >= 0)
			return true;
		if ((*it)->ASI == 0 && res == 0)
			return true;
	}
	return false;
}

// Compares two AIDs stored as byte arrays. Returns:
	//  0 - AIDs are identical
	//  1 - AIDs are identical up to the lenghth of the terminal AID (len_term)
	// -1 - AIDs are different
int ApplSelector::IsSameAID (const byte *aid_icc, int len_icc, 
		const byte *aid_term, int len_term)
{
	if (len_term > len_icc)
		return -1; // Different
	else
	{
		for (int i = 0; i < len_term; i++)
		{
			if (aid_icc[i] != aid_term[i])
				return -1;
		}
		if (len_icc == len_term)
			return 0; // arrays are identical
		else
			return 1; // arrays are identical up to the length of the len_term
	}
}

// Recursivley goes through the directory structure, finds all matching to 
	// the terminal ADFs, and adds them to the Canidate list
int ApplSelector::SelectDirectories (byte SFI)
{
	int res;
	if (m_TransactionToken == 0)
		return NO_TRANSACTION;

	scr_command command(&SCR);

	// Read all records for this directory
	PARSER_LIST RecordStack;
	if ((res = ReadRecords (SFI, command, RecordStack)) != SUCCESS)
	{
		return res;
	}

	// Per each record do processing
	while (!RecordStack.empty () && m_TransactionToken != 0)
	{
		tlv_parser *tlv_rec = RecordStack.front ();
				
		tlv_parser *tlv_pseDir = tlv_rec->Find(0x70);
		if (!tlv_pseDir)
		{
			// No directory entry is found
			delete [] tlv_rec->GetTlvDataObject ();
			delete tlv_rec;
			RecordStack.pop_front();
			continue; // Continue with the next record
		}
			
		// For each directory entry in a current record
		tlv_parser *tlv_recEntry = tlv_pseDir->FindFirst (0x61);
		while (tlv_recEntry && m_TransactionToken != 0)
		{
			tlv_parser *tlv_Entry = tlv_recEntry->Find (0x9D, false);
			if (!tlv_Entry)
			{
				tlv_Entry = tlv_recEntry->Find (0x4F, false);
				if (!tlv_Entry)
				{
					// Unrecognized Directory entry. 
					// Directory Entry must have either DDF Directory 
					// entry (tag '9D'), or ADF Dir Entry (tag '4F').
					// Exit transaction.
					ResetList(RecordStack);
					return ERR_INVALID_ICC_DATA;
				}
				// This is ADF entry

				// Check the template format
				if ((res = CheckADFTemplate (tlv_recEntry)) != SUCCESS)
				{
					ResetList(RecordStack);
					return res;
				}

				// Check if AID of this entry is in a list of terminal AID's
				if (FindAID (tlv_Entry->GetRoot()->GetValue (), 
							 tlv_Entry->GetRoot()->GetLengthVal (),
							 &ApplList))
				{
					// Terminal supports this AID; add it to the candidate list
					tlv_parser *tlv_appl = new tlv_parser();
					byte *data = tlv_recEntry->GetRoot ()->CopyDataObject();
					tlv_appl->parse (data, tlv_recEntry->GetRoot()->GetDataObjectLen());
					CandList.push_back (tlv_appl);
				}
			}
			else // (tlv_Entry != NULL)
			{
				// This is DDF Entry
				if ((res = CheckDDFTemplate (tlv_recEntry)) != SUCCESS)
				{
					ResetList(RecordStack);
					return res;
				}

				// Select the directory with ADF_Name
				command.setSelect(tlv_Entry->GetRoot ()->GetValue (),
								  tlv_Entry->GetRoot ()->GetLengthVal (),
								  true, true);
				R_APDU r_dir;
				// Execute SELECT command
				if ((res = command.run(&r_dir, m_TransactionToken)) != SUCCESS)
				{
					// Error executing command; Exit the function
					ResetList(RecordStack);
					return res;
				}
				if (r_dir.getSW1 () != 0x90 || r_dir.getSW2() != 0x00)
				{
					// Error in selecting the directory; continue with the
					// next entry in the record
					tlv_recEntry = tlv_pseDir->FindNext ();
					continue;
				}
				// Directory is selected successfully
				tlv_parser tlv_direct;
				byte *dir_data = r_dir.copyData ();
				if ((res = tlv_direct.parse (dir_data, r_dir.getDataLen ())) 
					!= SUCCESS)
				{
					// Error parsing the data returned by the SELECT command;
					delete [] dir_data;
					ResetList(RecordStack);
					return res;
				}
				// Check template
				byte sfi;
				if ((res = CheckFCITemplateDir(false, &tlv_direct, &sfi)) !=
					SUCCESS)
				{
					// Invalid template format
					delete [] dir_data;
					ResetList(RecordStack);
					return res;
				}
				// Recursivly call this function to continue traversing a directory
				// structure
				if ((res = SelectDirectories (sfi)) != SUCCESS)
				{
					// Error encountered while traversing the directory;
					// Stop traversing and exit the function
					ResetList(RecordStack);
					delete [] dir_data;
					return res;
				}
				delete [] dir_data;
			}
			tlv_recEntry = tlv_pseDir->FindNext ();
		}
		delete [] tlv_rec->GetTlvDataObject();
		delete tlv_rec;
		RecordStack.pop_front ();
	}
	ResetList(RecordStack);
	if (m_TransactionToken == 0)
		return TRANSACTION_CANCELED;
	else
		return SUCCESS; // The only successful exit of the function
}

void ApplSelector::OutputMsg(const char* msg)
{
/*	#ifdef _DEBUG
		if (UI.opened ())
		{
			UI.writeStatus (msg);
		}
	#endif*/
	printf("ApplSelector::OutputMsg(): %s\n",msg);
}

// Checks the data of DDF Directory Entry Format
// EMV book 1, section 8.2.3, table 42.
int ApplSelector::CheckDDFTemplate (tlv_parser *tlv_ddf)
{
	tlv_parser *tlv_item;
	
	// Check DDF Name - Mandatory
	tlv_item = tlv_ddf->Find (0x9D, false);
	if (!tlv_item)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;

	// All the rest fields are optional, so Don't check them
	
	return SUCCESS;
}

// Checks the data of ADF Directory Entry Format
// EMV book 1, section 8.2.3, table 43
int ApplSelector::CheckADFTemplate (tlv_parser *tlv_adf)
{
	tlv_parser *tlv_item;
	
	// Check ADF Name - Mandatory
	tlv_item = tlv_adf->Find (0x4F, false);
	if (!tlv_item)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;
	
	// Check Application Label - Mandatory
	tlv_item = tlv_adf->Find (0x50, false);
	if (!tlv_item)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;
	
	// All the rest are optional fields, so don't check them
	
	return SUCCESS;
}

// Checks the data of PSE or DDF FCI Format
// EMV book 1, section 7.3.4, tables 38 and 39
int ApplSelector::CheckFCITemplateDir (bool pse, 
									   tlv_parser *tlv_dir, 
									   byte *sfi)
{
	tlv_parser *tlv_item;
	tlv_parser *tlv_fci;
	
	// Check FCI Template - Mandatory
	if (tlv_dir->GetRoot ()->GetTagVal () != 0x6f)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;
	
	// Check DF Name - Mandatory
	tlv_item = tlv_dir->Find (0x84, false);
	if (!tlv_item)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;
	
	// Check FCI template -- Mandatory
	tlv_fci = tlv_dir->Find (0xa5, false);
	if (!tlv_fci)
		return EMV_MISSING_MANDATORY_DATA;
	
	// Check SFI -- Mandatory
	tlv_item = tlv_fci->Find (0x88, false);
	if (!tlv_item)
		return EMV_MISSING_MANDATORY_DATA;
	*sfi = tlv_item->GetRoot ()->GetValue ()[0];
	if (*sfi < 1 || *sfi > 10)
		return ERR_INVALID_ICC_DATA;

	if (pse)
	{
		// Check if the Issuer Code Table Index has valid value if it is present
		byte CodeIndx = 0;
		if (GetIssuerCodeIndx(tlv_fci, &CodeIndx) == ERR_INVALID_ICC_DATA)
			return ERR_INVALID_ICC_DATA;
		// Set Issuer Code Table Index for this PSE directory
		pseIssuerCodeIndx = CodeIndx;
	}
	// All the rest is optional data, so don't do additional checking
	
	return SUCCESS;
}

// Checks the data of ADF FCI Format
// EMV book 1, section 7.3.4, table 40
int ApplSelector::CheckFCITemplateAdf (tlv_parser *tlv_dir, 
									   tlv_parser **tlv_DFName)
{
	tlv_parser *tlv_item;
	tlv_parser *tlv_fci;
	
	// Check FCI Template - Mandatory
	if (tlv_dir->GetRoot ()->GetTagVal () != 0x6f)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;
	
	// Check DF Name - Mandatory
	tlv_item = tlv_dir->Find (0x84, false);
	if (!tlv_item)
		return EMV_MISSING_MANDATORY_DATA; //ERR_INVALID_ICC_DATA;
	if (tlv_DFName)
		*tlv_DFName = tlv_item;
	
	// Check FCI template -- Mandatory
	tlv_fci = tlv_dir->Find (0xa5, false);
	if (!tlv_fci)
		return EMV_MISSING_MANDATORY_DATA;

	// Check if the Issuer Code Table Index has valid value if it is present
	byte CodeIndx;
	if (GetIssuerCodeIndx(tlv_fci, &CodeIndx) == ERR_INVALID_ICC_DATA)
		return ERR_INVALID_ICC_DATA;

	// All the rest are optional data objects, so don't check them

	return SUCCESS;
}

bool ApplSelector::IsPSESupported ()
{
	CnfgOperationEventImpl cnfgOpEvent;

	int res  = SUCCESS;
/*
	res = CNFG.addOperationEvent (&cnfgOpEvent);
	if (res != SUCCESS)
	{
		//Cannot add Operation Event
		return false;
	}

	cnfgOpEvent.resetEvent (true);

	res = CNFG.getValue (CNFG_TERMINAL, "EnablePSE", "DirectorySelection");
	CNFG.removeEvent ();
	if (res == SUCCESS)
	{
		if (cnfgOpEvent.getValueType() != OPEVENT_LONG)
			return false;
		
		long pseFlg;
		cnfgOpEvent.getLong (&pseFlg);
		if (pseFlg != 0)
		{
			// PSE is enabled; check the value of PSE directory name
			if (!btPSE)
			{
				GetPSE();
				if (!btPSE)
				{
					// Cannot retreive the name of PSE dir name from registry;
					// use default value:
					resetPSE();
					btPSE = new byte [14];
					if (!btPSE)
						return false;

					memcpy(btPSE, "1PAY.SYS.DDF01", 14);
					PSE_len = 14;
				}
			}
			return true;
		}
	}
*/
	resetPSE();
	btPSE = new byte [14];
	if (!btPSE)
		return false;
	
	memcpy(btPSE, "1PAY.SYS.DDF01", 14);
	PSE_len = 14;
	return true;
}
