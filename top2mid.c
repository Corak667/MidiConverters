// Tales of Phantasia SPC -> Midi Converter
// ----------------------------------------
// Written by Valley Bell, 2012, 2014, 2016

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <stdtype.h>
#include <stdbool.h>

#ifdef _MSC_VER
#define stricmp	_stricmp
#else
#define stricmp	strcasecmp
#endif


#include "midi_funcs.h"

typedef struct _track_info
{
	UINT16 startOfs;
	UINT16 loopOfs;
	UINT32 tickCnt;
	UINT32 loopTick;
	UINT16 loopTimes;
	UINT8 mode;
} TRK_INF;

#define HAS_RUN_NOTE_STRUCT
typedef struct running_note
{
	UINT8 midChn;
	UINT8 orgNote;	// orignal Note value - used to extend notes
	UINT8 note;
	UINT8 velOff;	// note off velocity
	UINT32 remLen;
} RUN_NOTE;

//#define RUNNING_NOTES	// we need a custom implementation of this here
#define BALANCE_TRACK_TIMES
#include "midi_utils.h"


#define INS_MAX_INSTRUMENTS	0x100
#define INS_DATA_BLK_LINES	1
#define INS_LINE_CNT		(INS_MAX_INSTRUMENTS * INS_DATA_BLK_LINES)

typedef struct instrument_setup
{
	UINT8 MidiIns;
	INT8 NoteMove;
	UINT8 DrumBase;
} INS_SETUP;

typedef struct channel_data_seq
{
	UINT8 Ins;
	INT8 Move;
	UINT8 DrmNote;
	UINT8 DefaultChn;
} CHN_DATA_SEQ;

typedef struct channel_data_midi
{
	UINT8 Vol;
	UINT8 Pan;
	UINT8 Expr;
	//UINT16 Pitch;
	UINT8 PBRange;
	UINT8 RPN_MSB;
	UINT8 RPN_LSB;
} CHN_DATA_MID;
typedef struct channel_data_drums
{
	UINT8 Pitch;
	UINT8 Volume;
	UINT8 Pan;
} CHN_DATA_DRM;


void ReadConvData(const char* FileName);
INS_SETUP* GetInsSetupData(UINT8 Ins, UINT8 Note);
void WriteRPN(FILE_INF* fInf, MID_TRK_STATE* MTS,
			  UINT8 RpnMSB, UINT8 RpnLSB, UINT8 RpnData, CHN_DATA_MID* MidChnData);
UINT8 ToP2Mid(void);
static void PreparseSeq(TRK_INF* trkInf, UINT16 BasePtr);
static void CheckRunningNotes(FILE_INF* fInf, UINT32* delay, UINT16* runNoteCnt, RUN_NOTE* runNotes);
static void FlushRunningNotes(FILE_INF* fInf, UINT32* delay, UINT16* runNoteCnt, RUN_NOTE* runNotes);
static UINT8 MidiDelayHandler(FILE_INF* fInf, UINT32* delay);
static UINT16 ReadLE16(const UINT8* Data);
static UINT32 Tempo2Mid(UINT16 bpm, UINT8 scale);
static double Lin2DB(UINT8 LinVol);
static UINT8 DB2Mid(double DB);


UINT16 MIDI_RES = 24;
UINT32 SpcLen;
UINT8* SpcData;
UINT32 MidLen;
UINT8* MidData;
UINT16 RunNoteCnt;
RUN_NOTE RunNotes[0x100];
INS_SETUP InsSetup[INS_LINE_CNT];
bool FixInsSet;
bool FixVolume;
bool WriteDbgCtrls;
bool LoopExt;
UINT8 DefLoopCur;

int main(int argc, char* argv[])
{
	FILE* hFile;
	char* StrPtr;
	char TempArr[0x08];
	int RetVal;
	
	printf("ToP SPC -> Midi Converter\n-------------------------\n");
	if (argc < 4)
	{
		printf("Usage: top2mid.exe Options Song.spc Song.mid [InsMap.ini]\n");
		printf("Options: (options can be combined)\n");
		printf("    r   Raw conversion (other options are ignored)\n");
		printf("    i   fix Instruments (needs InsMap.ini)\n");
		printf("    v   fix Volume (convert linear SNES to logarithmic MIDI)\n");
		printf("    d   write Debug Controllers (for unknown events)\n");
		printf("    x   no loop extension\n");
		printf("Supported games: Tales Of Phantasia SFC and Star Ocean.\n");
		return 0;
	}
	
	MidiDelayCallback = MidiDelayHandler;
	
	FixInsSet = false;
	FixVolume = false;
	WriteDbgCtrls = false;
	LoopExt = true;
	DefLoopCur = 2;
	
	StrPtr = argv[1];
	while(*StrPtr != '\0')
	{
		switch(toupper(*StrPtr))
		{
		case 'R':
			FixInsSet = false;
			FixVolume = false;
			break;
		case 'I':
			FixInsSet = true;
			break;
		case 'V':
			FixVolume = true;
			break;
		case 'D':
			WriteDbgCtrls = true;
			break;
		case 'X':
			LoopExt = false;
			break;
		}
		StrPtr ++;
	}
	
	if (FixInsSet)
	{
		if (argc <= 4)
		{
			printf("Insufficient arguments!\n");
			return 9;
		}
		
		ReadConvData(argv[4]);
	}
	
	hFile = fopen(argv[2], "rb");
	if (hFile == NULL)
	{
		printf("Error opening file!\n");
		return 1;
	}
	
	fread(TempArr, 0x01, 0x08, hFile);
	if (strncmp(TempArr, "SNES-SPC", 0x08))
	{
		fclose(hFile);
		printf("Not an SPC file!\n");
		return 2;
	}
	
	fseek(hFile, 0x100, SEEK_SET);	// jump to SPC RAM dump
	
	SpcLen = 0x10000;	// 64 KB
	SpcData = (UINT8*)malloc(SpcLen);
	fread(SpcData, 0x01, SpcLen, hFile);
	
	fclose(hFile);
	
	RetVal = ToP2Mid();
	if (RetVal)
		return 3;
	
	hFile = fopen(argv[3], "wb");
	if (hFile == NULL)
	{
		printf("Error opening file!\n");
		return 1;
	}
	
	fwrite(MidData, 0x01, MidLen, hFile);
	
	fclose(hFile);
	printf("Done.\n");
	
#ifdef _DEBUG
	getchar();
#endif
	
	return 0;
}

void ReadConvData(const char* FileName)
{
	FILE* hFile;
	char TempStr[0x100];
	char* TempPnt;
	char* EndPnt;
	UINT16 InsNum;
	UINT8 InsLine;
	INS_SETUP* TempIns;
	
	hFile = fopen(FileName, "rt");
	if (hFile == NULL)
	{
		printf("Error opening %s\n", FileName);
		return;
	}
	
	TempIns = InsSetup;
	for (InsNum = 0x00; InsNum < INS_MAX_INSTRUMENTS; InsNum ++)
	{
		for (InsLine = 0x00; InsLine < INS_DATA_BLK_LINES; InsLine ++, TempIns ++)
		{
			TempIns->MidiIns = 0xFF/*InsNum*/;
			TempIns->NoteMove = 0x00;
			TempIns->DrumBase = 0xFF;
		}
	}
	
	while(! feof(hFile))
	{
		TempPnt = fgets(TempStr, 0x100, hFile);
		if (TempPnt == NULL)
			break;
		if (TempStr[0x00] == '\n' || TempStr[0x00] == '\0')
			continue;
		if (TempStr[0x00] == ';')
		{
			// skip comment lines
			// fgets has set a null-terminator at char 0xFF
			while(TempStr[strlen(TempStr) - 1] != '\n')
			{
				fgets(TempStr, 0x100, hFile);
				if (TempStr[0x00] == '\0')
					break;
			}
			continue;
		}
		
		TempPnt = strchr(TempStr, '\t');
		if (TempPnt == NULL || TempPnt == TempStr)
			continue;	// invalid line
		
		InsNum = (UINT8)strtoul(TempStr, &EndPnt, 0x10);
		if (EndPnt == TempStr || InsNum >= INS_MAX_INSTRUMENTS)
			continue;
		
		if (EndPnt != NULL && *EndPnt == '-')
			InsLine = (UINT8)strtoul(EndPnt + 1, NULL, 0x10);
		else
			InsLine = 0x00;
		//InsLine &= 0x0F;
		
		//TempIns = &InsSetup[(InsNum << 4) | InsLine];
		TempIns = &InsSetup[InsNum];
		TempPnt ++;
		if (*TempPnt != 'D')
		{
			TempIns->MidiIns = (UINT8)strtoul(TempPnt, NULL, 0x10);
			TempPnt = strchr(TempPnt, '\t');
			if (TempPnt != NULL)
			{
				TempPnt ++;
				TempIns->NoteMove = (UINT8)strtoul(TempPnt, NULL, 10);
			}
			else
			{
				TempIns->NoteMove = 0x00;
			}
			TempIns->DrumBase = 0xFF;
		}
		else
		{
			TempPnt ++;
			TempIns->MidiIns = 0x80;
			TempIns->NoteMove = (UINT8)strtoul(TempPnt, NULL, 0x10);
			TempPnt = strchr(TempPnt, '\t');
			if (TempPnt != NULL)
			{
				TempPnt ++;
				TempIns->DrumBase = (UINT8)strtoul(TempPnt, NULL, 0x10);
			}
			else
			{
				TempIns->DrumBase = 0x00;
			}
			if (! TempIns->DrumBase)
				TempIns->DrumBase = TempIns->NoteMove;
		}
	}
	
	fclose(hFile);
	
	return;
}

INS_SETUP* GetInsSetupData(UINT8 Ins, UINT8 Note)
{
	INS_SETUP* TempIS;
	
	TempIS = &InsSetup[Ins];
	if (TempIS->MidiIns == 0xFF)
	{
		printf("Warning: Unmapped instrument %02X!\n", Ins);
		TempIS->MidiIns = Ins & 0x7F;
	}
	return TempIS;
}

void WriteRPN(FILE_INF* fInf, MID_TRK_STATE* MTS,
			  UINT8 RpnMSB, UINT8 RpnLSB, UINT8 RpnData, CHN_DATA_MID* MidChnData)
{
	UINT8 RPNCtrl;
	
	if (RpnMSB & 0x80)
		RPNCtrl = 0x62;	// NRPN
	else
		RPNCtrl = 0x64;	// RPN
	
	// sadly, this doesn't work, because I process every track separately
//	if (MidChnData->RPN_MSB != RpnMSB || MidChnData->RPN_LSB != RpnLSB)
	{
		WriteEvent(fInf, MTS, 0xB0, RPNCtrl | 0x01, RpnMSB & 0x7F);
		WriteEvent(fInf, MTS, 0xB0, RPNCtrl | 0x00, RpnLSB & 0x7F);
		MidChnData->RPN_MSB = RpnMSB;
		MidChnData->RPN_LSB = RpnLSB;
	}
	
	if (! (RpnData & 0x80))
		WriteEvent(fInf, MTS, 0xB0, 0x06, RpnData);
	
	return;
}


#define TRK_COUNT	0x0F
UINT8 ToP2Mid(void)
{
	static const UINT8 DRIVER_SIG[0x14] =
	{	0x20, 0xE8, 0x00, 0xC4, 0xF4, 0xC4, 0xF5, 0xC4,
		0xF6, 0xC4, 0xF7, 0xC4, 0x83, 0x8F, 0x30, 0xF1,
		0xCD, 0xFF, 0xBD, 0x3F};
	UINT16 BasePtr;
	TRK_INF TrkInfo[TRK_COUNT];
	TRK_INF* TempTInf;
	UINT8 CurTrk;
	UINT16 SegBase;
	UINT16 SegIdx;
	UINT16 InPos;
	FILE_INF midFileInf;
	MID_TRK_STATE MTS;
	bool TrkEnd;
	UINT8 CurCmd;
	
	UINT8 LoopIdx;
	UINT16 LoopCur[0x10];	// current loop counter (16-bit due to loop extension)
	UINT16 LoopPos[0x10];	// loop offset
	UINT16 LoopSeg[0x10];	// loop segment
	CHN_DATA_SEQ ChnSeq;
	CHN_DATA_MID ChnMid;
	CHN_DATA_DRM ChnDrm[0x80];
	INS_SETUP* TempIS;
	CHN_DATA_DRM* TempChD;
	UINT8 TempArr[0x04];
	UINT32 TempLng;
	INT16 TempSSht;
	UINT8 TempByt;
	UINT8 CurNote;
	UINT8 LastChn;
	UINT8 NoteVol;
	UINT8 ChnVol;
	//INT8 NoteMove;
	//UINT8 DrmNote;
	//UINT8 PBRange;
	UINT8 CurDrmPtch;
	
	UINT8 MsgMask;
	UINT8 InitTempo;
	
	if (memcmp(&SpcData[0x0840], DRIVER_SIG, 0x14))
	{
		printf("This SPC uses an unsupported sound driver!\n");
		return 0xFF;
	}
	BasePtr = ReadLE16(&SpcData[0x0854]);
	
	midFileInf.alloc = 0x40000;	// 256 KB should be enough (so-37 has 66.4 KB)
	midFileInf.data = (UINT8*)malloc(midFileInf.alloc);
	midFileInf.pos = 0x00;
	
	WriteMidiHeader(&midFileInf, 0x0001, 1 + TRK_COUNT, MIDI_RES);
	
	InPos = BasePtr + 0x0020;
	
	WriteMidiTrackStart(&midFileInf, &MTS);
	MTS.midChn = 0x00;
	
	InPos += 0x02;	// skip file size
	
	InitTempo = SpcData[InPos];
	TempLng = Tempo2Mid(InitTempo, 0x40);
	WriteBE32(TempArr, TempLng);
	WriteMetaEvent(&midFileInf, &MTS, 0x51, 0x03, &TempArr[0x01]);
	InPos ++;
	
	if (FixInsSet)
		WriteEvent(&midFileInf, &MTS, 0xC9, 0x00, 0x00);	// make sure that the drum settings are reset
	
	WriteEvent(&midFileInf, &MTS, 0xFF, 0x2F, 0x00);
	WriteMidiTrackEnd(&midFileInf, &MTS);
	
	
	for (CurTrk = 0x00; CurTrk < TRK_COUNT; CurTrk ++, InPos += 0x03)
	{
		TempTInf = &TrkInfo[CurTrk];
		TempTInf->mode = SpcData[InPos + 0x00];
		TempTInf->startOfs = ReadLE16(&SpcData[InPos + 0x01]);
		TempTInf->loopOfs = 0x0000;
		TempTInf->tickCnt = 0;
		TempTInf->loopTick = 0;
		
		PreparseSeq(TempTInf, BasePtr);
		TempTInf->loopTimes = TempTInf->loopOfs ? DefLoopCur : 0;
	}
	if (LoopExt)
		BalanceTrackTimes(TRK_COUNT, TrkInfo, MIDI_RES / 4, 0xFF);
	
	for (CurTrk = 0x00; CurTrk < TRK_COUNT; CurTrk ++)
	{
		TempTInf = &TrkInfo[CurTrk];
		SegBase = BasePtr + TempTInf->startOfs;
		
		WriteMidiTrackStart(&midFileInf, &MTS);
		
		TrkEnd = ! (TempTInf->mode >> 7);
		LoopIdx = 0x00;
		MTS.midChn = CurTrk + (CurTrk + 6) / 15;
		LastChn = MTS.midChn;
		NoteVol = 0x7F;
		ChnVol = 0x64;
		//DrmNote = 0x00;
		//NoteMove = 0x00;
		RunNoteCnt = 0x00;
		MsgMask = 0x00;
		SegIdx = 0x00;
		InPos = 0x0000;
		//PBRange = 0x00;
		
		ChnSeq.Ins = 0xFF;
		ChnSeq.Move = 0;
		ChnSeq.DrmNote = 0xFF;
		ChnSeq.DefaultChn = MTS.midChn;
		CurDrmPtch = 0xFF;
		//ChnMid.Pitch = 0x4000;
		ChnMid.PBRange = 0x00;
		ChnMid.RPN_MSB = 0x7F;	// RPN Null
		ChnMid.RPN_LSB = 0x7F;
		for (TempByt = 0x00; TempByt < 0x80; TempByt ++)
		{
			TempChD = &ChnDrm[TempByt];
			TempChD->Pitch = 0x40;
			TempChD->Volume = 0x7F;
			TempChD->Pan = 0x40;
		}
		
		while(! TrkEnd)
		{
			if (InPos == 0x0000)
			{
				//if (SegIdx == 0xFFFF)
				//	break;
				InPos = ReadLE16(&SpcData[SegBase + SegIdx * 0x02]);
				if (InPos == 0xFFFF)
					break;
				InPos += BasePtr;
				SegIdx ++;
			}
			
			CurCmd = SpcData[InPos];
			if (CurCmd < 0x90)
			{
				if (ChnSeq.DrmNote == 0xFF)
				{
					// normal Melody instrument note
					CurNote = CurCmd + ChnSeq.Move;
					TempSSht = (INT16)CurCmd + ChnSeq.Move;
					if (TempSSht < 0x00)
						CurNote = 0x00;
					else if (TempSSht > 0x7F)
						CurNote = 0x7F;
					else
						CurNote = (UINT8)TempSSht;
				}
				else
				{
					TempIS = GetInsSetupData(ChnSeq.Ins, CurCmd);
					if (TempIS->DrumBase != 0xFF)
					{
						// single Drum instrument with varying pitch
						// -> convert to NRPN: Drum Pitch + Drum Note
						TempChD = &ChnDrm[ChnSeq.DrmNote];
						// The "default" pitch is 0x40. 00..3F is lower, 41..7F is higher.
						TempSSht = (INT16)CurCmd - TempIS->DrumBase + 0x40;
						if (TempSSht < 0x00)
							TempByt = 0x00;
						else if (TempSSht > 0x7F)
							TempByt = 0x7F;
						else
							TempByt = (UINT8)TempSSht;
						
						if (TempChD->Pitch != TempByt)
						{
							// NRPN: Drum Note Pitch
							WriteRPN(&midFileInf, &MTS, 0x98, ChnSeq.DrmNote, TempByt, &ChnMid);
							TempChD->Pitch = TempByt;
						}
						
						//CurNote = ChnSeq.DrmNote;
						CurNote = TempIS->NoteMove;
					}
					else
					{
						// instrument with multiple drums
						// This would need a lookup table.
						CurNote = TempIS->NoteMove;
					}
					CurNote |= 0x80;
				}
				
				if (FixVolume)
					NoteVol = DB2Mid(Lin2DB(SpcData[InPos + 0x03]));
				else
					NoteVol = SpcData[InPos + 0x03] >> 1;
				
				CheckRunningNotes(&midFileInf, &MTS.curDly, &RunNoteCnt, RunNotes);
				
				for (TempByt = 0x00; TempByt < RunNoteCnt; TempByt ++)
				{
					if (RunNotes[TempByt].orgNote == CurCmd)
					{
						RunNotes[TempByt].remLen = MTS.curDly + SpcData[InPos + 0x02];
						break;
					}
				}
				if (TempByt >= RunNoteCnt)
				{
					WriteEvent(&midFileInf, &MTS, 0x90, CurNote & 0x7F, NoteVol);
					if (RunNoteCnt < 0x80)
					{
						RunNotes[RunNoteCnt].midChn = MTS.midChn;
						RunNotes[RunNoteCnt].orgNote = CurCmd;
						RunNotes[RunNoteCnt].note = CurNote;
						RunNotes[RunNoteCnt].velOff = 0x80;
						RunNotes[RunNoteCnt].remLen = SpcData[InPos + 0x02];
						RunNoteCnt ++;
					}
				}
				
				MTS.curDly += SpcData[InPos + 0x01];
				InPos += 0x04;
			}
			else
			{
				switch(CurCmd)
				{
				case 0x90:	// Delay
					MTS.curDly += SpcData[InPos + 0x01];
					InPos += 0x02;
					break;
				case 0x92:	// Loop Start
					if (InPos == TempTInf->loopOfs)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6F, 0x00);
					else if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x70, LoopIdx);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, 0x00);
					}
					InPos += 0x01;
					
					LoopSeg[LoopIdx] = SegIdx;
					LoopPos[LoopIdx] = InPos;
					LoopCur[LoopIdx] = 0x00;
					LoopIdx ++;
					break;
				case 0x93:	// Loop End
					TempByt = SpcData[InPos + 0x01];	// loop count
					InPos += 0x02;
					
					if (! LoopIdx)
					{
						// missing Loop Start (see ToP 219)
						printf("Trk %u: Warning: Loop End without Loop Start at 0x%04X\n", CurTrk, InPos - 0x02);
						//LoopSeg[LoopIdx] = 0x01;	// start from beginning of the 2nd segment
						//LoopPos[LoopIdx] = 0x0000;
						//LoopCur[LoopIdx] = 0x00;
						//LoopIdx ++;
						if (! TempByt)
							WriteEvent(&midFileInf, &MTS, 0xB0, 0x6F, 0x01);
						else if (WriteDbgCtrls)
						{
							WriteEvent(&midFileInf, &MTS, 0xB0, 0x70, LoopIdx);
							WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, 0x7F);
						}
						break;
					}
					LoopIdx --;
					LoopCur[LoopIdx] ++;
					if (! TempByt && LoopCur[LoopIdx] <= 0x7F)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6F, (UINT8)LoopCur[LoopIdx]);
					else if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x70, LoopIdx);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, LoopCur[LoopIdx] & 0x7F);
					}
					/*if (! TempByt && LoopCur[LoopIdx] >= 0x02)
					{
						SegIdx = 0xFFFF;
						break;
					}*/
					if (LoopCur[LoopIdx] < TempByt ||
						(! TempByt && LoopCur[LoopIdx] < TempTInf->loopTimes))
					{
						SegIdx = LoopSeg[LoopIdx];
						InPos = LoopPos[LoopIdx];
						LoopIdx ++;
					}
					
					break;
				case 0x94:	// Pitch Bend
					TempByt = 12;
					if (ChnMid.PBRange != TempByt)
					{
						WriteRPN(&midFileInf, &MTS, 0x00, 0x00, TempByt, &ChnMid);
						ChnMid.PBRange = TempByt;
					}
					
					WriteEvent(&midFileInf, &MTS, 0xE0, 0x00, SpcData[InPos + 0x02]);
					MTS.curDly += SpcData[InPos + 0x01];
					InPos += 0x03;
					break;
				case 0x95:	// Set Tempo (relative to initial tempo)
					TempByt = SpcData[InPos + 0x02];
					TempLng = Tempo2Mid(InitTempo, TempByt);	// RCP-stype tempo
					WriteBE32(TempArr, TempLng);
					WriteMetaEvent(&midFileInf, &MTS, 0x51, 0x03, &TempArr[0x01]);
					
					if (WriteDbgCtrls)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x02]);
					
					MTS.curDly += SpcData[InPos + 0x01];
					InPos += 0x03;
					break;
				case 0x96:	// Set Instrument
					TempByt = SpcData[InPos + 0x01];
					ChnSeq.Ins = TempByt;
					if (FixInsSet)
					{
						TempIS = GetInsSetupData(ChnSeq.Ins, 0xFF);
						TempByt = TempIS->MidiIns & 0x7F;
						
						// MidiIns 80..FF -> Drum Note
						if ((TempIS->MidiIns & 0x80) && TempIS->DrumBase != 0xFF)
						{
							// Drum Instrument: one drum with varying pitch
							ChnSeq.DrmNote = (UINT8)TempIS->NoteMove;
							
#if 0
							if (CurDrmPtch == 0xFF)
							{
								WriteEvent(&midFileInf, &MTS, 0xB0, 0x00, 0x7F);
								WriteEvent(&midFileInf, &MTS, 0xB0, 0x20, 0x00);
								WriteEvent(&midFileInf, &MTS, 0xC0, 0x00, 0x00);
								
								WriteEvent(&midFileInf, &MTS, 0xB0, 0x07, 0x7F);
								CurDrmPtch = 0x00;
							}
#else
							if (MTS.midChn != 0x09)
							{
								MTS.midChn = 0x09;
								// copy Volume/Pan to Drum Channel
								WriteRPN(&midFileInf, &MTS,
									0x9A, ChnSeq.DrmNote, ChnMid.Vol, &ChnMid);	// Drum Volume
								WriteRPN(&midFileInf, &MTS,
									0x9C, ChnSeq.DrmNote, ChnMid.Pan, &ChnMid);	// Drum Panorama
							}
#endif
						}
						else
						{
							if ((TempIS->MidiIns & 0x80) && TempIS->DrumBase == 0xFF)
							{
								// generic Drum Channel
								MTS.midChn = 0x09;
								ChnSeq.Move = 0;
								ChnSeq.DrmNote = (UINT8)TempIS->NoteMove;
							}
							else
							{
								// normal Instrument
								if (MTS.midChn == 0x09)
								{
									// copy Volume/Pan back to Normal Channel
									MTS.midChn = ChnSeq.DefaultChn;
									WriteEvent(&midFileInf, &MTS, 0xB0, 0x07, ChnMid.Vol);	// Volume
									WriteEvent(&midFileInf, &MTS, 0xB0, 0x0A, ChnMid.Pan);	// Panorama
								}
								MTS.midChn = ChnSeq.DefaultChn;
								ChnSeq.Move = TempIS->NoteMove;
								ChnSeq.DrmNote = 0xFF;
							}
						}
					}
					
					if (ChnSeq.DrmNote == 0xFF)
						WriteEvent(&midFileInf, &MTS, 0xC0, TempByt, 0x00);
					InPos += 0x02;
					break;
				case 0x97:	// another Volume setting?
				case 0x98:	// Set Volume
					if (FixVolume)
						TempByt = DB2Mid(Lin2DB(SpcData[InPos + 0x02]));
					else
						TempByt = SpcData[InPos + 0x02] >> 1;
					if (CurCmd == 0x97)
					{
						ChnMid.Vol = TempByt;
						if (ChnSeq.DrmNote == 0xFF)
							WriteEvent(&midFileInf, &MTS, 0xB0, 0x07, ChnMid.Vol);
						else
							WriteRPN(&midFileInf, &MTS, 0x9A, ChnSeq.DrmNote, ChnMid.Vol, &ChnMid);
					}
					else //if (CurCmd == 0x98)
					{
						ChnMid.Expr = TempByt;
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x0B, ChnMid.Expr);
					}
					MTS.curDly += SpcData[InPos + 0x01];
					InPos += 0x03;
					break;
				case 0x99:	// Set Pan
					ChnMid.Pan = 0x80 - SpcData[InPos + 0x02];
					if (ChnMid.Pan >= 0x80)
						ChnMid.Pan = 0x7F;
					if (ChnSeq.DrmNote == 0xFF)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x0A, ChnMid.Pan);
					else
						WriteRPN(&midFileInf, &MTS, 0x9C, ChnSeq.DrmNote, ChnMid.Pan, &ChnMid);
					MTS.curDly += SpcData[InPos + 0x01];
					InPos += 0x03;
					break;
				case 0x9B:
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
					}
					InPos += 0x02;
					break;
				case 0x9C:	// Modulation
					// Parameters: Tick Delay, Depth, Time
					//ModDelay = SpcData[InPos + 0x01]
					//ModVal = SpcData[InPos + 0x02] * 4
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x02]);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x03]);
					}
					InPos += 0x04;
					break;
				case 0xA2:	// Detune
					if (WriteDbgCtrls)
					{
						/*WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);*/
					}
					WriteRPN(&midFileInf, &MTS, 0x00, 0x01, SpcData[InPos + 0x01], &ChnMid);
					WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, 0x00);
					InPos += 0x02;
					break;
				case 0xA3:
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
					}
					InPos += 0x02;
					break;
				case 0xAA:	// set Reverb/Chorus
					//WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
					if (0 || WriteDbgCtrls)
					{
						// These are global settings, so I write them to all channels.
						UINT8 chnBak = MTS.midChn;
						for (MTS.midChn = 0x00; MTS.midChn < 0x10; MTS.midChn ++)
						{
							WriteEvent(&midFileInf, &MTS, 0xB0, 0x5B, SpcData[InPos + 0x01]);
							WriteEvent(&midFileInf, &MTS, 0xB0, 0x5D, SpcData[InPos + 0x02]);
						}
						MTS.midChn = chnBak;
					}
					InPos += 0x03;
					break;
				case 0xAD:
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
					}
					InPos += 0x02;
					break;
				case 0xAE:
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
					}
					InPos += 0x02;
					break;
				case 0xAF:
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x02]);
					}
					InPos += 0x03;
					break;
				case 0xB2:
					if (WriteDbgCtrls)
					{
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x26, SpcData[InPos + 0x01]);
					}
					InPos += 0x02;
					break;
				case 0xC8:
					if (WriteDbgCtrls)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
					InPos += 0x01;
					break;
				case 0xF0:
					if (WriteDbgCtrls)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
					InPos += 0x01;
					break;
				case 0xFD:	// Segment Return
					//InPos += 0x01;
					InPos = 0x0000;
					break;
				case 0xFE:
					if (WriteDbgCtrls)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6D, CurCmd & 0x7F);
					InPos += 0x01;
					break;
				case 0xFF:
					TrkEnd = true;
					InPos += 0x01;
					break;
				default:
					printf("Unknown event %02X on track %X at %04X\n", SpcData[InPos + 0x00], CurTrk, InPos);
					if (WriteDbgCtrls)
						WriteEvent(&midFileInf, &MTS, 0xB0, 0x6E, CurCmd & 0x7F);
					InPos += 0x01;
					TrkEnd = true;
					break;
				}
			}
		}
		FlushRunningNotes(&midFileInf, &MTS.curDly, &RunNoteCnt, RunNotes);
		
		WriteEvent(&midFileInf, &MTS, 0xFF, 0x2F, 0x00);
		
		WriteMidiTrackEnd(&midFileInf, &MTS);
	}
	MidData = midFileInf.data;
	MidLen = midFileInf.pos;
	
	return 0x00;
}

static void PreparseSeq(TRK_INF* trkInf, UINT16 BasePtr)
{
	UINT16 SegBase;
	UINT16 SegIdx;
	UINT16 InPos;
	UINT32 CurTick;
	bool TrkEnd;
	UINT8 CurCmd;
	
	UINT8 LoopIdx;
	UINT8 LoopCur[0x10];
	UINT16 LoopPos[0x10];
	UINT16 LoopSeg[0x10];
	UINT32 LoopTick[0x10];
	UINT8 LoopCount;
	
	SegBase = BasePtr + trkInf->startOfs;
	
	TrkEnd = ! (trkInf->mode >> 7);
	LoopIdx = 0x00;
	SegIdx = 0x00;
	InPos = 0x0000;
	CurTick = 0;
	
	while(! TrkEnd)
	{
		if (InPos == 0x0000)
		{
			InPos = ReadLE16(&SpcData[SegBase + SegIdx * 0x02]);
			if (InPos == 0xFFFF)
				break;
			InPos += BasePtr;
			SegIdx ++;
		}
		
		CurCmd = SpcData[InPos];
		if (CurCmd < 0x90)
		{
			CurTick += SpcData[InPos + 0x01];
			InPos += 0x04;
		}
		else
		{
			switch(CurCmd)
			{
			case 0x90:	// Delay
				CurTick += SpcData[InPos + 0x01];
				InPos += 0x02;
				break;
			case 0x92:	// Loop Start
				InPos += 0x01;
				
				LoopSeg[LoopIdx] = SegIdx;
				LoopPos[LoopIdx] = InPos;
				LoopCur[LoopIdx] = 0x00;
				LoopTick[LoopIdx] = CurTick;
				LoopIdx ++;
				break;
			case 0x93:	// Loop End
				LoopCount = SpcData[InPos + 0x01];
				InPos += 0x02;
				
				if (! LoopIdx)
					break;	// missing Loop Start
				LoopIdx --;
				LoopCur[LoopIdx] ++;
				
				if (! LoopCount)	// infinite loop
				{
					trkInf->loopOfs = LoopPos[LoopIdx] - 0x01;
					trkInf->loopTick = LoopTick[LoopIdx];
					TrkEnd = true;
				}
				if (LoopCur[LoopIdx] < LoopCount)
				{
					SegIdx = LoopSeg[LoopIdx];
					InPos = LoopPos[LoopIdx];
					LoopIdx ++;
				}
				break;
			case 0x94:	// Pitch Bend
				CurTick += SpcData[InPos + 0x01];
				InPos += 0x03;
				break;
			case 0x95:	// Set Tempo (relative to initial tempo)
				CurTick += SpcData[InPos + 0x01];
				InPos += 0x03;
				break;
			case 0x96:	// Set Instrument
				InPos += 0x02;
				break;
			case 0x97:	// another Volume setting?
				CurTick += SpcData[InPos + 0x01];
				InPos += 0x03;
				break;
			case 0x98:	// Set Volume
				CurTick += SpcData[InPos + 0x01];
				InPos += 0x03;
				break;
			case 0x99:	// Set Pan
				CurTick += SpcData[InPos + 0x01];
				InPos += 0x03;
				break;
			case 0x9B:
				InPos += 0x02;
				break;
			case 0x9C:
				InPos += 0x04;
				break;
			case 0xA2:	// Detune
				InPos += 0x02;
				break;
			case 0xA3:
				InPos += 0x02;
				break;
			case 0xAA:	// set Reverb/Chorus
				InPos += 0x03;
				break;
			case 0xAD:
				InPos += 0x02;
				break;
			case 0xAE:
				InPos += 0x02;
				break;
			case 0xAF:
				InPos += 0x03;
				break;
			case 0xB2:
				InPos += 0x02;
				break;
			case 0xC8:
				InPos += 0x01;
				break;
			case 0xF0:
				InPos += 0x01;
				break;
			case 0xFD:	// Segment Return
				//InPos += 0x01;
				InPos = 0x0000;
				break;
			case 0xFE:
				InPos += 0x01;
				break;
			case 0xFF:
				TrkEnd = true;
				InPos += 0x01;
				break;
			default:
				InPos += 0x01;
				TrkEnd = true;
				break;
			}
		}
	}
	trkInf->tickCnt = CurTick;
	
	return;
}

static void CheckRunningNotes(FILE_INF* fInf, UINT32* delay, UINT16* runNoteCnt, RUN_NOTE* runNotes)
{
	UINT8 CurNote;
	UINT32 TempDly;
	RUN_NOTE* TempNote;
	
	while(*runNoteCnt)
	{
		// 1. Check if we're going beyond a note's timeout.
		TempDly = *delay + 1;
		for (CurNote = 0x00; CurNote < *runNoteCnt; CurNote ++)
		{
			TempNote = &runNotes[CurNote];
			if (TempNote->remLen < TempDly)
				TempDly = TempNote->remLen;
		}
		if (TempDly >= *delay)
			break;	// not beyond the timeout - do the event
		
		// 2. advance all notes by X ticks
		for (CurNote = 0x00; CurNote < *runNoteCnt; CurNote ++)
			runNotes[CurNote].remLen -= TempDly;
		(*delay) -= TempDly;
		
		// 3. send NoteOff for expired notes
		for (CurNote = 0x00; CurNote < *runNoteCnt; CurNote ++)
		{
			TempNote = &runNotes[CurNote];
			if (! TempNote->remLen)	// turn note off, it going beyond the Timeout
			{
				WriteMidiValue(fInf, TempDly);
				TempDly = 0;
				
				File_CheckRealloc(fInf, 0x03);
				fInf->data[fInf->pos + 0x00] = 0x90 | TempNote->midChn;
				fInf->data[fInf->pos + 0x01] = TempNote->note & 0x7F;
				fInf->data[fInf->pos + 0x02] = 0x00;
				fInf->pos += 0x03;
				
				if (TempNote->note == (0x80 | 0x2E))
				{
					File_CheckRealloc(fInf, 0x03 + 0x03);
					WriteMidiValue(fInf, 0);
					fInf->data[fInf->pos + 0x00] = 0x2C;
					fInf->data[fInf->pos + 0x01] = 0x01;
					fInf->pos += 0x02;
					
					WriteMidiValue(fInf, 0);
					fInf->data[fInf->pos + 0x00] = 0x2C;
					fInf->data[fInf->pos + 0x01] = 0x00;
					fInf->pos += 0x02;
				}
				
				(*runNoteCnt) --;
				if (*runNoteCnt)
					*TempNote = runNotes[*runNoteCnt];
				CurNote --;
			}
		}
	}
	
	return;
}

static void FlushRunningNotes(FILE_INF* fInf, UINT32* delay, UINT16* runNoteCnt, RUN_NOTE* runNotes)
{
	UINT16 curNote;
	
	for (curNote = 0; curNote < *runNoteCnt; curNote ++)
	{
		if (runNotes[curNote].remLen > *delay)
			*delay = runNotes[curNote].remLen;	// set "delay" to longest note
	}
	(*delay) ++;
	CheckRunningNotes(fInf, delay, runNoteCnt, runNotes);
	(*delay) --;
	
	return;
}

static UINT8 MidiDelayHandler(FILE_INF* fInf, UINT32* delay)
{
	CheckRunningNotes(fInf, delay, &RunNoteCnt, RunNotes);
	if (*delay)
	{
		UINT16 curNote;
		
		for (curNote = 0; curNote < RunNoteCnt; curNote ++)
			RunNotes[curNote].remLen -= *delay;
	}
	
	return 0x00;
}

static UINT16 ReadLE16(const UINT8* Data)
{
	return (Data[0x01] << 8) | (Data[0x00] << 0);
}

static UINT32 Tempo2Mid(UINT16 bpm, UINT8 scale)
{
	// formula: (60 000 000 / bpm) * (64 / scale) * (MIDI_RES / 48)
	UINT32 div = bpm * scale;
	return (UINT32)((UINT64)80000000 * MIDI_RES / div);
}

static double Lin2DB(UINT8 LinVol)
{
	//return log(LinVol / 255.0) / log(2.0) * 6.0;
	return log(LinVol / 255.0) * 8.65617024533378;
}

static UINT8 DB2Mid(double DB)
{
	return (UINT8)(pow(10.0, DB / 40.0) * 0x7F + 0.5);
}
