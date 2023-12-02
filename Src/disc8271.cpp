/****************************************************************
BeebEm - BBC Micro and Master 128 Emulator
Copyright (C) 1994  David Alan Gilbert
Copyright (C) 1997  Mike Wyatt

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public
License along with this program; if not, write to the Free
Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA  02110-1301, USA.
****************************************************************/

// 04/12/1994 David Alan Gilbert: 8271 disc emulation
// 30/08/1997 Mike Wyatt: Added disc write and format support
// 27/12/2011 J.G.Harston: Double-sided SSD supported

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "disc8271.h"
#include "6502core.h"
#include "UEFState.h"
#include "beebsound.h"
#include "sysvia.h"
#include "tube.h"
#include "Log.h"
#include "DiscType.h"
#include "Main.h"

#define ENABLE_LOG 0

// 8271 Status register
const unsigned char STATUS_REG_COMMAND_BUSY       = 0x80;
const unsigned char STATUS_REG_COMMAND_FULL       = 0x40;
const unsigned char STATUS_REG_PARAMETER_FULL     = 0x20;
const unsigned char STATUS_REG_RESULT_FULL        = 0x10;
const unsigned char STATUS_REG_INTERRUPT_REQUEST  = 0x08;
const unsigned char STATUS_REG_NON_DMA_MODE       = 0x04;

// 8271 Result register
const unsigned char RESULT_REG_SUCCESS                = 0x00;
const unsigned char RESULT_REG_SCAN_NOT_MET           = 0x00;
const unsigned char RESULT_REG_SCAN_MET_EQUAL         = 0x02;
const unsigned char RESULT_REG_SCAN_MET_NOT_EQUAL     = 0x04;
const unsigned char RESULT_REG_CLOCK_ERROR            = 0x08;
const unsigned char RESULT_REG_LATE_DMA               = 0x0A;
const unsigned char RESULT_REG_ID_CRC_ERRORV          = 0x0C;
const unsigned char RESULT_REG_DATA_CRC_ERROR         = 0x0E;
const unsigned char RESULT_REG_DRIVE_NOT_READY        = 0x10;
const unsigned char RESULT_REG_WRITE_PROTECT          = 0x12;
const unsigned char RESULT_REG_TRACK_0_NOT_FOUND      = 0x14;
const unsigned char RESULT_REG_WRITE_FAULT            = 0x16;
const unsigned char RESULT_REG_SECTOR_NOT_FOUND       = 0x18;
const unsigned char RESULT_REG_DRIVE_NOT_PRESENT      = 0x1E; // Undocumented, see http://beebwiki.mdfs.net/OSWORD_%267F
const unsigned char RESULT_REG_DELETED_DATA_FOUND     = 0x20;
const unsigned char RESULT_REG_DELETED_DATA_CRC_ERROR = 0x2E;

// 8271 special registers
const unsigned char SPECIAL_REG_SCAN_SECTOR_NUMBER        = 0x06;
const unsigned char SPECIAL_REG_SCAN_COUNT_MSB            = 0x14;
const unsigned char SPECIAL_REG_SCAN_COUNT_LSB            = 0x13;
const unsigned char SPECIAL_REG_SURFACE_0_CURRENT_TRACK   = 0x12;
const unsigned char SPECIAL_REG_SURFACE_1_CURRENT_TRACK   = 0x1A;
const unsigned char SPECIAL_REG_MODE_REGISTER             = 0x17;
const unsigned char SPECIAL_REG_DRIVE_CONTROL_OUTPUT_PORT = 0x23;
const unsigned char SPECIAL_REG_DRIVE_CONTROL_INPUT_PORT  = 0x22;
const unsigned char SPECIAL_REG_SURFACE_0_BAD_TRACK_1     = 0x10;
const unsigned char SPECIAL_REG_SURFACE_0_BAD_TRACK_2     = 0x11;
const unsigned char SPECIAL_REG_SURFACE_1_BAD_TRACK_1     = 0x18;
const unsigned char SPECIAL_REG_SURFACE_1_BAD_TRACK_2     = 0x19;

bool Disc8271Enabled = true;
int Disc8271Trigger; // Cycle based time Disc8271Trigger

static int DriveHeadPosition[2]={0};
static bool DriveHeadLoaded=false;
static bool DriveHeadUnloadPending=false;

static unsigned char PositionInTrack; // FSD
static bool SectorOverRead; // FSD - Was read size bigger than data stored?
static bool UsingSpecial; // FSD - Using Special Register
static unsigned char DRDSC; // FSD

constexpr int TRACKS_PER_DRIVE = 80;
constexpr int FSD_TRACKS_PER_DRIVE = 40 + 1;

// Note: reads/writes one byte every 80us
constexpr int TIME_BETWEEN_BYTES = 160;

struct IDFieldType {
	// Cylinder Number byte which identifies the track number
	unsigned char LogicalTrack;
	// Head Number byte which specifies the head used (top or bottom)
	// to access the sector
	unsigned char HeadNum;
	// Record Number byte identifying the sector number
	unsigned char LogicalSector;
	// The byte length of the sector
	unsigned char SectorLength;
};

struct SectorType {
	IDFieldType IDField;
	unsigned char CylinderNum; // FSD - moved from IDField
	unsigned char RecordNum; // FSD - moved from IDField
	int IDSiz; // FSD - 2 bytes for size, could be calculated automatically?
	int RealSectorSize; // FSD - moved from IDField, PhysRecLength
	unsigned char Error; // FSD - error code when sector was read, 20 for deleted data
	unsigned char *Data;
};

struct TrackType {
	int LogicalSectors; // Number of sectors stated in format command
	int NSectors; // i.e. the number of records we have - not anything physical
	SectorType *Sectors;
	int Gap1Size; // From format command
	int Gap3Size;
	int Gap5Size;
	bool TrackIsReadable; // FSD - is the track readable, or just contains track ID?
};

struct DiscStatusType {
	DiscType Type;
	char FileName[256]; // File name of loaded disc image
	bool Writeable; // True if the disc is writeable
	int NumHeads; // Number of sides of loaded disc images
	int TotalTracks; // Total number of tracks in FSD disk image
	TrackType Tracks[2][TRACKS_PER_DRIVE]; // All data on the disc - first param head, then physical track ID
};

static DiscStatusType DiscStatus[2];

struct FDCStateType {
	unsigned char ResultReg;
	unsigned char StatusReg;
	unsigned char DataReg;

	unsigned char Command;
	int CommandParamCount;
	int CurrentParam; // From 0
	unsigned char Params[16]; // Wildly more than we need

	// These bools indicate which drives the last command selected.
	// They also act as "drive ready" bits which are reset when the motor stops.
	bool Select[2]; // Drive selects

	unsigned char ScanSectorNum;
	unsigned int ScanCount; // Read as two bytes
	unsigned char ModeReg;
	unsigned char CurrentTrack[2]; // 0/1 for surface number
	unsigned char DriveControlOutputPort;
	unsigned char DriveControlInputPort;
	unsigned char BadTracks[2][2]; // 1st subscript is surface 0/1 and second subscript is badtrack 0/1

	// State set by the Specify (initialisation) command
	// See Intel 8271 data sheet, page 15, ADUG page 39-40
	int StepRate; // In 2ms steps
	int HeadSettlingTime; // In 2ms steps
	int IndexCountBeforeHeadUnload; // Number of revolutions (0 to 14), or 15 to keep loaded
	int HeadLoadTime; // In 8ms steps
};

static FDCStateType FDCState;

// Note Head select is done from bit 5 of the drive output register
#define CURRENT_HEAD ((FDCState.DriveControlOutputPort >> 5) & 1)

unsigned char FSDLogicalTrack;
unsigned char FSDPhysicalTrack;

static bool SaveTrackImage(int DriveNum, int HeadNum, int TrackNum);
static void DriveHeadScheduleUnload(void);

/*--------------------------------------------------------------------------*/

struct CommandStatusType {
	int TrackAddr;
	int CurrentSector;
	int SectorLength; // In bytes
	int SectorsToGo;

	SectorType *CurrentSectorPtr;
	TrackType *CurrentTrackPtr;

	int ByteWithinSector; // Next byte in sector or ID field
	bool FirstWriteInt; // Indicates the start of a write operation
	unsigned char NextInterruptIsErr; // non-zero causes error and drops this value into result reg
};

static CommandStatusType CommandStatus;

/*--------------------------------------------------------------------------*/

static void UpdateNMIStatus()
{
	if (FDCState.StatusReg & STATUS_REG_INTERRUPT_REQUEST)
	{
		NMIStatus |= 1 << nmi_floppy;
	}
	else
	{
		NMIStatus &= ~(1 << nmi_floppy);
	}
}

/*--------------------------------------------------------------------------*/

// For appropriate commands checks the select bits in the command code and
// selects the appropriate drive.

static void DoSelects()
{
	FDCState.Select[0] = (FDCState.Command & 0x40) != 0;
	FDCState.Select[1] = (FDCState.Command & 0x80) != 0;

	FDCState.DriveControlOutputPort &= 0x3f;

	if (FDCState.Select[0]) FDCState.DriveControlOutputPort |= 0x40;
	if (FDCState.Select[1]) FDCState.DriveControlOutputPort |= 0x80;
}

/*--------------------------------------------------------------------------*/

static void NotImp(const char *Command)
{
	mainWin->Report(MessageType::Error,
	                "Disc operation '%s' not supported", Command);
}

/*--------------------------------------------------------------------------*/

// Load the head - ignore for the moment

static void DoLoadHead()
{
}

/*--------------------------------------------------------------------------*/

// Initialise our disc structures

static void InitDiscStore()
{
	const TrackType Blank = { 0, 0, nullptr, 0, 0, 0, false };

	for (int Drive = 0; Drive < 2; Drive++)
	{
		for (int Head = 0; Head < 2; Head++)
		{
			for (int Track = 0; Track < TRACKS_PER_DRIVE; Track++)
			{
				DiscStatus[Drive].Tracks[Head][Track] = Blank;
			}
		}
	}
}

/*--------------------------------------------------------------------------*/

// Given a logical track number accounts for bad tracks

static int SkipBadTracks(int Unit, int trackin)
{
	/* int offset=0;
	if (TubeType != Tube::TorchZ80) // If running under Torch Z80, ignore bad tracks
	{
		if (Internal_BadTracks[Unit][0]<=trackin) offset++;
		if (Internal_BadTracks[Unit][1]<=trackin) offset++;
	}
	return(trackin+offset); */

	return trackin; // FSD - no bad tracks, but possible to have unformatted
}

/*--------------------------------------------------------------------------*/

static int GetSelectedDrive()
{
	if (FDCState.Select[0])
	{
		return 0;
	}

	if (FDCState.Select[1])
	{
		return 1;
	}

	return -1;
}

/*--------------------------------------------------------------------------*/

// Returns a pointer to the data structure for a particular track. You pass
// the logical track number, it takes into account bad tracks and the drive
// select and head select etc.  It always returns a valid ptr - if there
// aren't that many tracks then it uses the last one.
// The one exception!!!! is that if no drives are selected it returns nullptr
// FSD - returns the physical track pointer for track ID

static TrackType *GetTrackPtrPhysical(unsigned char PhysicalTrackID)
{
	int Drive = GetSelectedDrive();

	if (Drive < 0) return nullptr;

	PositionInTrack = 0;
	FSDPhysicalTrack = PhysicalTrackID;

	return &DiscStatus[Drive].Tracks[CURRENT_HEAD][PhysicalTrackID];
}

/*--------------------------------------------------------------------------*/

// Returns a pointer to the data structure for a particular track.  You pass
// the logical track number, it takes into account bad tracks and the drive
// select and head select etc.  It always returns a valid ptr - if there
// there aren't that many tracks then it uses the last one.
// The one exception!!!! is that if no drives are selected it returns nullptr

// FSD - unsigned char because maximum &FF

static TrackType *GetTrackPtr(unsigned char LogicalTrackID) {
  int Drive = GetSelectedDrive();

  if (Drive < 0)
  {
    return nullptr;
  }

  // Read two tracks extra
  for (unsigned char Track = FSDPhysicalTrack; Track < FSDPhysicalTrack +  2; Track++) {
    SectorType *SecPtr = DiscStatus[Drive].Tracks[CURRENT_HEAD][Track].Sectors;

    // Fixes Krakout!
    if (SecPtr == nullptr)
    {
      return nullptr;
    }

    if (LogicalTrackID == SecPtr[0].IDField.LogicalTrack) {
      FSDPhysicalTrack = Track;
      return &DiscStatus[Drive].Tracks[CURRENT_HEAD][FSDPhysicalTrack];
     }
  }

  return nullptr; // if it's not found from the above, then it doesn't exist!
}

/*--------------------------------------------------------------------------*/

// Returns a pointer to the data structure for a particular sector.
// Returns nullptr for Sector not found. Doesn't check cylinder/head ID.

static SectorType *GetSectorPtr(TrackType *Track, unsigned char LogicalSectorID, bool FindDeleted) {

  // if (Track->Sectors == nullptr) return nullptr;

  // FSD - from PositionInTrack, instead of 0 to allow Mini Office II to have repeated sector ID
  // if logical sector from track ID is logicalsectorid passed here then return the record number
  // and move the positionintrack to here too

  for (int CurrentSector = PositionInTrack; CurrentSector < Track->NSectors; CurrentSector++) {
    if (Track->Sectors[CurrentSector].IDField.LogicalSector == LogicalSectorID) {
      LogicalSectorID = Track->Sectors[CurrentSector].RecordNum;
      PositionInTrack = Track->Sectors[CurrentSector].RecordNum;
      return &Track->Sectors[LogicalSectorID];
    }
  }

  // As above, but from sector 0 to the current position
  if (PositionInTrack > 0) {
    for (int CurrentSector = 0; CurrentSector < PositionInTrack; CurrentSector++) {
      if (Track->Sectors[CurrentSector].IDField.LogicalSector == LogicalSectorID) {
        LogicalSectorID = Track->Sectors[CurrentSector].RecordNum;
        PositionInTrack = CurrentSector;
        return &Track->Sectors[LogicalSectorID];
      }
    }
  }

  return nullptr;
}

/*--------------------------------------------------------------------------*/

// Returns a pointer to the data structure for a particular sector.
// Returns nullptr for Sector not found. Doesn't check cylinder/head ID
// FSD - returns the sector IDs

static SectorType *GetSectorPtrForTrackID(TrackType *Track, unsigned char LogicalSectorID, bool FindDeleted)
{
	if (Track->Sectors == nullptr)
	{
		return nullptr;
	}

	LogicalSectorID = Track->Sectors[PositionInTrack].RecordNum;

	return &Track->Sectors[LogicalSectorID];
}

/*--------------------------------------------------------------------------*/

// Cause an error - pass err num

static void DoErr(unsigned char ErrNum)
{
	SetTrigger(50, Disc8271Trigger); // Give it a bit of time
	CommandStatus.NextInterruptIsErr = ErrNum;
	FDCState.StatusReg = STATUS_REG_COMMAND_BUSY; // Command is busy - come back when I have an interrupt
	UpdateNMIStatus();
}

/*--------------------------------------------------------------------------*/

// Checks a few things in the sector - returns true if OK
// FSD - Sectors are always OK

static bool ValidateSector(const SectorType *Sector, int Track, int SecLength)
{
	return true;
}

/*--------------------------------------------------------------------------*/

static void DoVarLength_ScanDataCommand()
{
	DoSelects();
	NotImp("DoVarLength_ScanDataCommand");
}

/*--------------------------------------------------------------------------*/

static void DoVarLength_ScanDataAndDeldCommand()
{
	DoSelects();
	NotImp("DoVarLength_ScanDataAndDeldCommand");
}

/*--------------------------------------------------------------------------*/

static void Do128ByteSR_WriteDataCommand()
{
	DoSelects();
	NotImp("Do128ByteSR_WriteDataCommand");
}

/*--------------------------------------------------------------------------*/
static void DoVarLength_WriteDataCommand(void) {
  DoSelects();
  DoLoadHead();

  const int Drive = GetSelectedDrive();

  if (Drive < 0) {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  if (!DiscStatus[Drive].Writeable) {
    DoErr(RESULT_REG_WRITE_PROTECT);
    return;
  }

  FDCState.CurrentTrack[Drive] = FDCState.Params[0];
  CommandStatus.CurrentTrackPtr = GetTrackPtr(FDCState.Params[0]);

  if (CommandStatus.CurrentTrackPtr == nullptr)
  {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr, FDCState.Params[1], false);

  if (CommandStatus.CurrentSectorPtr == nullptr)
  {
    DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
    return;
  }

  CommandStatus.TrackAddr     = FDCState.Params[0];
  CommandStatus.CurrentSector = FDCState.Params[1];
  CommandStatus.SectorsToGo   = FDCState.Params[2] & 31;
  CommandStatus.SectorLength  = 1 << (7 + ((FDCState.Params[2] >> 5) & 7));

  if (ValidateSector(CommandStatus.CurrentSectorPtr,CommandStatus.TrackAddr,CommandStatus.SectorLength)) {
    CommandStatus.ByteWithinSector=0;
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
    UpdateNMIStatus();
    CommandStatus.ByteWithinSector=0;
    CommandStatus.FirstWriteInt = true;
  } else {
    DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
  }
}

/*--------------------------------------------------------------------------*/
static void WriteInterrupt(void) {
  bool LastByte = false;

  if (CommandStatus.SectorsToGo < 0) {
    FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
    UpdateNMIStatus();
    return;
  }

  if (!CommandStatus.FirstWriteInt)
  {
    CommandStatus.CurrentSectorPtr->Data[CommandStatus.ByteWithinSector++] = FDCState.DataReg;
  }
  else
  {
    CommandStatus.FirstWriteInt = false;
  }

  FDCState.ResultReg = RESULT_REG_SUCCESS;

  if (CommandStatus.ByteWithinSector>=CommandStatus.SectorLength) {
    CommandStatus.ByteWithinSector=0;
    if (--CommandStatus.SectorsToGo) {
      CommandStatus.CurrentSector++;
      CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr,
                                                    CommandStatus.CurrentSector,
                                                    false);

      if (CommandStatus.CurrentSectorPtr == nullptr)
      {
        DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
        return;
      }
    } else {
      // Last sector done, write the track back to disc
      if (SaveTrackImage(FDCState.Select[0] ? 0 : 1, CURRENT_HEAD, CommandStatus.TrackAddr)) {
        FDCState.StatusReg = STATUS_REG_RESULT_FULL;
        UpdateNMIStatus();
        LastByte = true;
        CommandStatus.SectorsToGo=-1; // To let us bail out
        SetTrigger(0,Disc8271Trigger); // To pick up result
      }
      else {
        DoErr(RESULT_REG_WRITE_PROTECT);
      }
    }
  }

  if (!LastByte) {
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
                         STATUS_REG_INTERRUPT_REQUEST |
                         STATUS_REG_NON_DMA_MODE;
    UpdateNMIStatus();
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
  }
}

/*--------------------------------------------------------------------------*/

static void Do128ByteSR_WriteDeletedDataCommand()
{
	DoSelects();
	NotImp("Do128ByteSR_WriteDeletedDataCommand");
}

/*--------------------------------------------------------------------------*/

static void DoVarLength_WriteDeletedDataCommand()
{
	DoSelects();
	NotImp("DoVarLength_WriteDeletedDataCommand");
}

/*--------------------------------------------------------------------------*/

static void Do128ByteSR_ReadDataCommand()
{
	DoSelects();
	NotImp("Do128ByteSR_ReadDataCommand");
}

/*--------------------------------------------------------------------------*/
static void DoVarLength_ReadDataCommand(void) {
  DoSelects();
  DoLoadHead();

  SectorOverRead = false; // FSD - if read size was larger than data stored

  const int Drive = GetSelectedDrive();

  if (Drive < 0) {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  // Reset shift state if it was set by Run Disc
  if (mainWin->m_ShiftBooted) {
    mainWin->m_ShiftBooted = false;
    BeebKeyUp(0, 0);
  }

  // FSD - if special register is NOT being used to point to track
  if (!UsingSpecial) {
    FSDPhysicalTrack = FDCState.Params[0];
  }

  // if reading a new track, then reset position
  if (FSDLogicalTrack != FDCState.Params[0]) {
    PositionInTrack = 0;
  }

  FSDLogicalTrack = FDCState.Params[0];

  if (DRDSC > 1) {
    FSDPhysicalTrack = 0; // FSDLogicalTrack
  }

  DRDSC = 0;

  /* if (FSDLogicalTrack == 0) {
    FSDPhysicalTrack = 0;
  } */

  if (FSDPhysicalTrack == 0) {
    FSDPhysicalTrack = FSDLogicalTrack;
  }

  // fixes The Music System
  if (FSDLogicalTrack == FSDPhysicalTrack) {
    UsingSpecial = false;
  }

  CommandStatus.CurrentTrackPtr = GetTrackPtr(FSDLogicalTrack);

  // Internal_CurrentTrack[Drive]=Params[0];
  // CommandStatus.CurrentTrackPtr=GetTrackPtr(Params[0]);

  if (CommandStatus.CurrentTrackPtr == nullptr)
  {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  // FSD - if track contains no data
  if (!CommandStatus.CurrentTrackPtr->TrackIsReadable) {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr, FDCState.Params[1], false);

  if (CommandStatus.CurrentSectorPtr == nullptr)
  {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  // (Over)Reading Track 2, Sector 9 on 3D Pool should result in Sector Not Found
  if ((CommandStatus.CurrentSectorPtr->Error == 0xE0) &&
      (CommandStatus.CurrentSectorPtr->IDField.LogicalSector == 0x09) &&
      (CommandStatus.SectorLength > CommandStatus.CurrentSectorPtr->RealSectorSize)) {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  CommandStatus.TrackAddr     = FDCState.Params[0];
  CommandStatus.CurrentSector = FDCState.Params[1];
  CommandStatus.SectorsToGo   = FDCState.Params[2] & 31;
  CommandStatus.SectorLength  = 1 << (7 + ((FDCState.Params[2] >> 5) & 7));

  // FSD - if trying to read more data than is stored, Disc Duplicator 3
  if (CommandStatus.SectorLength > CommandStatus.CurrentSectorPtr->RealSectorSize) {
    CommandStatus.SectorLength = CommandStatus.CurrentSectorPtr->RealSectorSize;
    SectorOverRead = true;
  }

  if (ValidateSector(CommandStatus.CurrentSectorPtr,CommandStatus.TrackAddr,CommandStatus.SectorLength)) {
    CommandStatus.ByteWithinSector=0;
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
    UpdateNMIStatus();
  } else {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
  }
}

/*--------------------------------------------------------------------------*/
static void ReadInterrupt(void) {
  bool LastByte = false;

  if (CommandStatus.SectorsToGo < 0) {
    FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
    UpdateNMIStatus();
    return;
  }

  FDCState.DataReg = CommandStatus.CurrentSectorPtr->Data[CommandStatus.ByteWithinSector++];

  #if ENABLE_LOG
  WriteLog("ReadInterrupt called - DataReg=0x%02X ByteWithinSector=%d\n", DataReg, CommandStatus.ByteWithinSector);
  #endif

  // FSD - use the error result from the FSD file
  FDCState.ResultReg = CommandStatus.CurrentSectorPtr->Error;

  // If track has no error, but the "real" size has not been read
  if (CommandStatus.CurrentSectorPtr->Error == RESULT_REG_SUCCESS &&
      CommandStatus.CurrentSectorPtr->RealSectorSize != CommandStatus.SectorLength)
  {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
  }

  if (SectorOverRead)
  {
    if (CommandStatus.CurrentSectorPtr->Error == RESULT_REG_SUCCESS)
    {
      FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
    }
    else if (CommandStatus.CurrentSectorPtr->Error == RESULT_REG_DELETED_DATA_FOUND)
    {
      FDCState.ResultReg = RESULT_REG_DELETED_DATA_CRC_ERROR;
    }
    else if (CommandStatus.CurrentSectorPtr->Error == RESULT_REG_DELETED_DATA_CRC_ERROR)
    {
      FDCState.ResultReg = RESULT_REG_DELETED_DATA_CRC_ERROR;
    }
  }

  // Same as above, but for deleted data
  if (CommandStatus.CurrentSectorPtr->Error == RESULT_REG_DELETED_DATA_FOUND &&
      CommandStatus.CurrentSectorPtr->RealSectorSize != CommandStatus.SectorLength) {
    FDCState.ResultReg = RESULT_REG_DELETED_DATA_CRC_ERROR;
  }

  if ((CommandStatus.CurrentSectorPtr->Error == RESULT_REG_DELETED_DATA_CRC_ERROR) &&
      (CommandStatus.CurrentSectorPtr->IDSiz == CommandStatus.SectorLength) && !SectorOverRead) {
    FDCState.ResultReg = RESULT_REG_DELETED_DATA_FOUND;
  }

  // If track has deliberate error, but the id field sector size has been read)
  if (CommandStatus.CurrentSectorPtr->Error == 0xE1 && CommandStatus.SectorLength != 0x100) {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
  }
  else if (CommandStatus.CurrentSectorPtr->Error == 0xE1 && CommandStatus.SectorLength == 0x100) {
    FDCState.ResultReg = RESULT_REG_SUCCESS;
  }

  if (CommandStatus.CurrentSectorPtr->Error == 0xE0 && CommandStatus.SectorLength != 0x80) {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
  }
  else if (CommandStatus.CurrentSectorPtr->Error == 0xE0 && CommandStatus.SectorLength == 0x80) {
    FDCState.ResultReg = RESULT_REG_SUCCESS;
  }

  if (CommandStatus.CurrentSectorPtr->Error == RESULT_REG_DATA_CRC_ERROR &&
      CommandStatus.CurrentSectorPtr->RealSectorSize == CommandStatus.CurrentSectorPtr->IDSiz)
  {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;

    if (CommandStatus.ByteWithinSector % 5 == 0) {
      FDCState.DataReg = FDCState.DataReg >> rand() % 8;
    }
  }

  if (CommandStatus.ByteWithinSector >= CommandStatus.SectorLength) {
    CommandStatus.ByteWithinSector = 0;
    // I don't know if this can cause the thing to step - I presume not for the moment
    if (--CommandStatus.SectorsToGo) {
      CommandStatus.CurrentSector++;
      CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr,
                                                    CommandStatus.CurrentSector,
                                                    false);

      if (CommandStatus.CurrentSectorPtr == nullptr)
      {
        DoErr(RESULT_REG_SECTOR_NOT_FOUND);
        return;
      }
    } else {
      // Last sector done
      FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
                           STATUS_REG_RESULT_FULL |
                           STATUS_REG_INTERRUPT_REQUEST |
                           STATUS_REG_NON_DMA_MODE;
      UpdateNMIStatus();
      LastByte = true;
      CommandStatus.SectorsToGo = -1; // To let us bail out
      SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger); // To pick up result
    }
  }

  if (!LastByte) {
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
                         STATUS_REG_INTERRUPT_REQUEST |
                         STATUS_REG_NON_DMA_MODE;
    UpdateNMIStatus();
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
  }
}

/*--------------------------------------------------------------------------*/
static void Do128ByteSR_ReadDataAndDeldCommand(void) {
  DoSelects();
  DoLoadHead();

  const int Drive = GetSelectedDrive();

  if (Drive < 0) {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  // FSD - if special register is NOT being used to point to logical track
  if (!UsingSpecial) {
    FSDPhysicalTrack = FDCState.Params[0];
  }

  FDCState.CurrentTrack[Drive] = FDCState.Params[0];

  // FSD - if internal track =0, seek track 0 too
  if (FDCState.CurrentTrack[Drive] == 0)
  {
    FSDPhysicalTrack = 0;
  }

  CommandStatus.CurrentTrackPtr = GetTrackPtr(FDCState.Params[0]);

  if (CommandStatus.CurrentTrackPtr == nullptr)
  {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  // FSD - if track contains no data
  if (!CommandStatus.CurrentTrackPtr->TrackIsReadable) {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr, FDCState.Params[1], false);

  if (CommandStatus.CurrentSectorPtr == nullptr)
  {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
    return;
  }

  CommandStatus.TrackAddr     = FDCState.Params[0];
  CommandStatus.CurrentSector = FDCState.Params[1];
  CommandStatus.SectorsToGo   = 1;
  CommandStatus.SectorLength  = 0x80;

  if (ValidateSector(CommandStatus.CurrentSectorPtr, CommandStatus.TrackAddr, CommandStatus.SectorLength)) {
    CommandStatus.ByteWithinSector = 0;
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
    UpdateNMIStatus();
    CommandStatus.ByteWithinSector = 0;
  }
  else {
    DoErr(RESULT_REG_SECTOR_NOT_FOUND);
  }
}

/*--------------------------------------------------------------------------*/

static void Read128Interrupt(void) {
  int LastByte = 0;

  if (CommandStatus.SectorsToGo < 0) {
    FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
    UpdateNMIStatus();
    return;
  }

  FDCState.DataReg = CommandStatus.CurrentSectorPtr->Data[CommandStatus.ByteWithinSector++];
  /*cerr << "ReadInterrupt called - DataReg=0x" << hex << int(DataReg) << dec << "ByteWithinSector=" << CommandStatus.ByteWithinSector << "\n"; */

  FDCState.ResultReg = CommandStatus.CurrentSectorPtr->Error; // FSD - used to be 0

  // if error is just deleted data, then result = 0
  // if (ResultReg==0x20) {ResultReg=0;}

  // If track has no error, but the "real" size has not been read
  if ((CommandStatus.CurrentSectorPtr->Error == 0) &&
      (CommandStatus.CurrentSectorPtr->RealSectorSize != CommandStatus.SectorLength)) {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
  }

  if (SectorOverRead) {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
  }

  // Same as above, but for deleted data
  if ((CommandStatus.CurrentSectorPtr->Error == 0x20) &&
      (CommandStatus.CurrentSectorPtr->RealSectorSize != CommandStatus.SectorLength)) {
    FDCState.ResultReg = RESULT_REG_DELETED_DATA_CRC_ERROR;
  }

  // If track has deliberate error, but the id field sector size has been read
  if ((CommandStatus.CurrentSectorPtr->Error == 0xE1) &&
      (CommandStatus.SectorLength != 0x100)) {
    FDCState.ResultReg = RESULT_REG_DATA_CRC_ERROR;
  }
  else if ((CommandStatus.CurrentSectorPtr->Error == 0xE1) &&
           (CommandStatus.SectorLength == 0x100)) {
    FDCState.ResultReg = RESULT_REG_SUCCESS;
  }

  if (CommandStatus.ByteWithinSector >= CommandStatus.SectorLength) {
    CommandStatus.ByteWithinSector = 0;
    // I don't know if this can cause the thing to step - I presume not for the moment
    if (--CommandStatus.SectorsToGo) {
      CommandStatus.CurrentSector++;
      CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr, CommandStatus.CurrentSector, false);

      if (CommandStatus.CurrentSectorPtr == nullptr)
      {
        DoErr(RESULT_REG_SECTOR_NOT_FOUND);
        return;
      }
    } else {
      // Last sector done
      FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
                           STATUS_REG_RESULT_FULL |
                           STATUS_REG_INTERRUPT_REQUEST |
                           STATUS_REG_NON_DMA_MODE;
      UpdateNMIStatus();
      LastByte = 1;
      CommandStatus.SectorsToGo = -1; // To let us bail out
      SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger); // To pick up result
    }
  }

  if (!LastByte) {
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
                         STATUS_REG_INTERRUPT_REQUEST |
                         STATUS_REG_NON_DMA_MODE;
    UpdateNMIStatus();
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
  }
}

/*--------------------------------------------------------------------------*/

static void DoVarLength_ReadDataAndDeldCommand()
{
	// Use normal read command for now - deleted data not supported
	DoVarLength_ReadDataCommand();
}

/*--------------------------------------------------------------------------*/

// The Read ID command transfers the specified number of ID fields Into
// memory (beginning with the first ID field after Index). The CRC character
// is checked but not transferred. These fields are entered into memory in the
// order in which they are physically located on the disk, with the first
// field being the one starting at the index pulse.
//
// The ID field is seven bytes long and is written for each sector when the
// track is formatted. Each ID field consists of:
//
// * an ID field Address Mark
// * a Cylinder Number byte which identifies the track number
// * a Head Number byte which specifies the head used (top or bottom) to access
//   the sector
// * a Record Number byte identifying the sector number (1 through 26 for
//   128 byte sectors)
// * an N-byte specifying the byte length of the sector
// * two CRC (Cyclic Redundancy Check) bytes
//
// Parameters:
// 0: Track Address
// 1: Zero
// 2: Number of ID Fields

static void DoReadIDCommand()
{
	DoSelects();
	DoLoadHead();

	const int Drive = GetSelectedDrive();

	if (Drive < 0)
	{
		DoErr(RESULT_REG_DRIVE_NOT_READY);
		return;
	}

	// Internal_CurrentTrack[Drive]=Params[0];
	FSDPhysicalTrack = FDCState.Params[0];
	CommandStatus.CurrentTrackPtr = GetTrackPtrPhysical(FSDPhysicalTrack);

	if (CommandStatus.CurrentTrackPtr == nullptr)
	{
		DoErr(RESULT_REG_SECTOR_NOT_FOUND);
		return;
	}

	// FSD - was GetSectorPtr
	CommandStatus.CurrentSectorPtr = GetSectorPtrForTrackID(CommandStatus.CurrentTrackPtr, 0, false);

	if (CommandStatus.CurrentSectorPtr == nullptr)
	{
		DoErr(RESULT_REG_SECTOR_NOT_FOUND);
		return;
	}

	CommandStatus.TrackAddr     = FDCState.Params[0];
	CommandStatus.CurrentSector = 0;
	CommandStatus.SectorsToGo   = FDCState.Params[2];

	if (CommandStatus.SectorsToGo == 0)
	{
		CommandStatus.SectorsToGo = 0x20;
	}

	CommandStatus.ByteWithinSector = 0;
	SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
	FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
	UpdateNMIStatus();

	// FSDPhysicalTrack = FSDPhysicalTrack + 1;
}

/*--------------------------------------------------------------------------*/

static void ReadIDInterrupt()
{
	bool LastByte = false;

	if (CommandStatus.SectorsToGo < 0)
	{
		FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
		UpdateNMIStatus();
		return;
	}

	if (CommandStatus.ByteWithinSector == 0)
	{
		FDCState.DataReg = CommandStatus.CurrentSectorPtr->IDField.LogicalTrack;
	}
	else if (CommandStatus.ByteWithinSector == 1)
	{
		FDCState.DataReg = CommandStatus.CurrentSectorPtr->IDField.HeadNum;
	}
	else if (CommandStatus.ByteWithinSector == 2)
	{
		FDCState.DataReg = CommandStatus.CurrentSectorPtr->IDField.LogicalSector; // RecordNum
	}
	else if (CommandStatus.ByteWithinSector == 3)
	{
		FDCState.DataReg = CommandStatus.CurrentSectorPtr->IDField.SectorLength; // was 1, for 256 byte
	}

	CommandStatus.ByteWithinSector++;

	FDCState.ResultReg = RESULT_REG_SUCCESS;

	if (CommandStatus.ByteWithinSector >= 4)
	{
		CommandStatus.ByteWithinSector = 0;

		if (--CommandStatus.SectorsToGo > 0)
		{
			if (++CommandStatus.CurrentSector == CommandStatus.CurrentTrackPtr->NSectors)
			{
				CommandStatus.CurrentSector = 0;
			}

			PositionInTrack = CommandStatus.CurrentSector; // FSD

			CommandStatus.CurrentSectorPtr = GetSectorPtrForTrackID(CommandStatus.CurrentTrackPtr,
			                                                        CommandStatus.CurrentSector,
			                                                        false);

			if (CommandStatus.CurrentSectorPtr == nullptr)
			{
				DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
				return;
			}
		}
		else
		{
			// Last sector done
			FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
			                     STATUS_REG_INTERRUPT_REQUEST |
			                     STATUS_REG_NON_DMA_MODE;
			UpdateNMIStatus();
			LastByte = true;
			// PositionInTrack=0; // FSD - track position to zero
			CommandStatus.SectorsToGo = -1; // To let us bail out
			SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger); // To pick up result
		}
	}

	if (!LastByte)
	{
		FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
		                     STATUS_REG_INTERRUPT_REQUEST |
		                     STATUS_REG_NON_DMA_MODE;
		UpdateNMIStatus();
		SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
	}
}

/*--------------------------------------------------------------------------*/

static void Do128ByteSR_VerifyDataAndDeldCommand()
{
	DoSelects();
	NotImp("Do128ByteSR_VerifyDataAndDeldCommand");
}

/*--------------------------------------------------------------------------*/
static void DoVarLength_VerifyDataAndDeldCommand(void) {
  DoSelects();

  const int Drive = GetSelectedDrive();

  if (Drive < 0) {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  FDCState.CurrentTrack[Drive] = FDCState.Params[0];
  FSDPhysicalTrack = FDCState.Params[0];
  FSDLogicalTrack = FDCState.Params[0];
  CommandStatus.CurrentTrackPtr = GetTrackPtr(FSDLogicalTrack);

  if (CommandStatus.CurrentTrackPtr == nullptr)
  {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr, FDCState.Params[1], false);

  if (CommandStatus.CurrentSectorPtr == nullptr)
  {
    DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
    return;
  }

  FDCState.ResultReg = CommandStatus.CurrentSectorPtr->Error;

  if (FDCState.ResultReg != 0) {
    FDCState.StatusReg = FDCState.ResultReg;
  }
  else {
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
  }

  UpdateNMIStatus();
  SetTrigger(100,Disc8271Trigger); // A short delay to causing an interrupt
}

/*--------------------------------------------------------------------------*/

static void VerifyInterrupt()
{
	FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
	UpdateNMIStatus();
	FDCState.ResultReg = RESULT_REG_SUCCESS; // All OK
}

/*--------------------------------------------------------------------------*/

static void DoFormatCommand(void) {
  DoSelects();

  DoLoadHead();

  const int Drive = GetSelectedDrive();

  if (Drive < 0) {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  if (!DiscStatus[Drive].Writeable) {
    DoErr(RESULT_REG_WRITE_PROTECT);
    return;
  }

  FDCState.CurrentTrack[Drive] = FDCState.Params[0];
  CommandStatus.CurrentTrackPtr=GetTrackPtr(FDCState.Params[0]);

  if (CommandStatus.CurrentTrackPtr == nullptr)
  {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr, 0, false);

  if (CommandStatus.CurrentSectorPtr == nullptr)
  {
    DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
    return;
  }

  CommandStatus.TrackAddr     = FDCState.Params[0];
  CommandStatus.CurrentSector = 0;
  CommandStatus.SectorsToGo   = FDCState.Params[2] & 31;
  CommandStatus.SectorLength  = 1 << (7 + ((FDCState.Params[2] >> 5) & 7));

  if (CommandStatus.SectorsToGo==10 && CommandStatus.SectorLength==256) {
    CommandStatus.ByteWithinSector=0;
    SetTrigger(TIME_BETWEEN_BYTES, Disc8271Trigger);
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
    UpdateNMIStatus();
    CommandStatus.FirstWriteInt = true;
  } else {
    DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
  }
}

/*--------------------------------------------------------------------------*/
static void FormatInterrupt(void) {
  bool LastByte = false;

  if (CommandStatus.SectorsToGo<0) {
    FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
    UpdateNMIStatus();
    return;
  }

  if (!CommandStatus.FirstWriteInt)
  {
    // Ignore the ID data for now - just count the bytes
    CommandStatus.ByteWithinSector++;
  }
  else
  {
    CommandStatus.FirstWriteInt = false;
  }

  FDCState.ResultReg = RESULT_REG_SUCCESS;

  if (CommandStatus.ByteWithinSector>=4) {
    // Fill sector with 0xe5 chars
    for (int i = 0; i < 256; ++i) {
      CommandStatus.CurrentSectorPtr->Data[i]=(unsigned char)0xe5;
    }

    CommandStatus.ByteWithinSector=0;
    if (--CommandStatus.SectorsToGo) {
      CommandStatus.CurrentSector++;
      CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr,
                                                    CommandStatus.CurrentSector,
                                                    false);

      if (CommandStatus.CurrentSectorPtr == nullptr)
      {
        DoErr(RESULT_REG_DRIVE_NOT_PRESENT); // Sector not found
        return;
      }
    } else {
      // Last sector done, write the track back to disc
      if (SaveTrackImage(FDCState.Select[0] ? 0 : 1, CURRENT_HEAD, CommandStatus.TrackAddr)) {
        FDCState.StatusReg = STATUS_REG_RESULT_FULL;
        UpdateNMIStatus();
        LastByte = true;
        CommandStatus.SectorsToGo=-1; // To let us bail out
        SetTrigger(0,Disc8271Trigger); // To pick up result
      }
      else {
        DoErr(RESULT_REG_WRITE_PROTECT);
      }
    }
  }

  if (!LastByte) {
    FDCState.StatusReg = STATUS_REG_COMMAND_BUSY |
                         STATUS_REG_INTERRUPT_REQUEST |
                         STATUS_REG_NON_DMA_MODE;
    UpdateNMIStatus();
    SetTrigger(TIME_BETWEEN_BYTES * 256, Disc8271Trigger);
  }
}

/*--------------------------------------------------------------------------*/

static void SeekInterrupt()
{
	FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
	UpdateNMIStatus();
	FDCState.ResultReg = RESULT_REG_SUCCESS; // All OK
}

/*--------------------------------------------------------------------------*/
static void DoSeekCommand(void) {
  DoSelects();

  DoLoadHead();

  int Drive = GetSelectedDrive();

  if (Drive<0) {
    DoErr(RESULT_REG_DRIVE_NOT_READY);
    return;
  }

  DRDSC = 0;
  FDCState.CurrentTrack[Drive] = FDCState.Params[0];
  FSDPhysicalTrack = FDCState.Params[0]; // FSD - where to start seeking data store
  UsingSpecial = false;
  PositionInTrack = 0;

  FDCState.StatusReg = STATUS_REG_COMMAND_BUSY;
  UpdateNMIStatus();
  SetTrigger(100,Disc8271Trigger); // A short delay to causing an interrupt
}

/*--------------------------------------------------------------------------*/
static void DoReadDriveStatusCommand(void) {
  bool Track0 = false;
  bool WriteProt = false;

  if (FDCState.Command & 0x40) {
    Track0 = FDCState.CurrentTrack[0] == 0;
    WriteProt = !DiscStatus[0].Writeable;
  }

  if (FDCState.Command & 0x80) {
    Track0 = FDCState.CurrentTrack[1] == 0;
    WriteProt = !DiscStatus[1].Writeable;
  }

  DRDSC++;
  FDCState.ResultReg = 0x80 | (FDCState.Select[1] ? 0x40 : 0)
                            | (FDCState.Select[0] ? 0x04 : 0)
                            | (WriteProt  ? 0x08 : 0)
                            | (Track0     ? 0x02 : 0);
  FDCState.StatusReg |= STATUS_REG_RESULT_FULL;
  UpdateNMIStatus();
}

/*--------------------------------------------------------------------------*/

// See Intel 8271 data sheet, page 15, ADUG page 39-40

static void DoSpecifyCommand(void) {
  switch (FDCState.Params[0]) {
    case 0x0D: // Initialisation
      FDCState.StepRate = FDCState.Params[1];
      FDCState.HeadSettlingTime = FDCState.Params[2];
      FDCState.IndexCountBeforeHeadUnload = (FDCState.Params[3] & 0xf0) >> 4;
      FDCState.HeadLoadTime = FDCState.Params[3] & 0x0f;
      break;

    case 0x10: // Load bad tracks, surface 0
      FDCState.BadTracks[0][0] = FDCState.Params[1];
      FDCState.BadTracks[0][1] = FDCState.Params[2];
      FDCState.CurrentTrack[0] = FDCState.Params[3];
      break;

    case 0x18: // Load bad tracks, surface 1
      FDCState.BadTracks[1][0] = FDCState.Params[1];
      FDCState.BadTracks[1][1] = FDCState.Params[2];
      FDCState.CurrentTrack[1] = FDCState.Params[3];
      break;
  }
}

/*--------------------------------------------------------------------------*/
static void DoWriteSpecialCommand(void) {
  DoSelects();

  switch (FDCState.Params[0]) {
    case SPECIAL_REG_SCAN_SECTOR_NUMBER:
      FDCState.ScanSectorNum = FDCState.Params[1];
      break;

    case SPECIAL_REG_SCAN_COUNT_MSB:
      FDCState.ScanCount &= 0xff;
      FDCState.ScanCount |= FDCState.Params[1] << 8;
      break;

    case SPECIAL_REG_SCAN_COUNT_LSB:
      FDCState.ScanCount &= 0xff00;
      FDCState.ScanCount |= FDCState.Params[1];
      break;

    case SPECIAL_REG_SURFACE_0_CURRENT_TRACK:
      FDCState.CurrentTrack[0] = FDCState.Params[1];
      FSDLogicalTrack = FDCState.Params[1];
      // FSD - using special register, so different track from seek
      UsingSpecial = FDCState.Params[1] != FSDPhysicalTrack;
      DRDSC = 0;
      break;

    case SPECIAL_REG_SURFACE_1_CURRENT_TRACK:
      FDCState.CurrentTrack[1] = FDCState.Params[1];
      break;

    case SPECIAL_REG_MODE_REGISTER:
      FDCState.ModeReg = FDCState.Params[1];
      break;

    case SPECIAL_REG_DRIVE_CONTROL_OUTPUT_PORT:
      FDCState.DriveControlOutputPort = FDCState.Params[1];
      FDCState.Select[0] = (FDCState.Params[1] & 0x40) != 0;
      FDCState.Select[1] = (FDCState.Params[1] & 0x80) != 0;
      break;

    case SPECIAL_REG_DRIVE_CONTROL_INPUT_PORT:
      FDCState.DriveControlInputPort = FDCState.Params[1];
      break;

    case SPECIAL_REG_SURFACE_0_BAD_TRACK_1:
      FDCState.BadTracks[0][0] = FDCState.Params[1];
      break;

    case SPECIAL_REG_SURFACE_0_BAD_TRACK_2:
      FDCState.BadTracks[0][1] = FDCState.Params[1];
      break;

    case SPECIAL_REG_SURFACE_1_BAD_TRACK_1:
      FDCState.BadTracks[1][0] = FDCState.Params[1];
      break;

    case SPECIAL_REG_SURFACE_1_BAD_TRACK_2:
      FDCState.BadTracks[1][1] = FDCState.Params[1];
      break;

    default:
      #if ENABLE_LOG
      WriteLog("Write to bad special register\n");
      #endif
      break;
  }
}

/*--------------------------------------------------------------------------*/
static void DoReadSpecialCommand(void) {
  DoSelects();

  switch (FDCState.Params[0]) {
    case SPECIAL_REG_SCAN_SECTOR_NUMBER:
      FDCState.ResultReg = FDCState.ScanSectorNum;
      break;

    case SPECIAL_REG_SCAN_COUNT_MSB:
      FDCState.ResultReg = (FDCState.ScanCount >> 8) & 0xff;
      break;

    case SPECIAL_REG_SCAN_COUNT_LSB:
      FDCState.ResultReg = FDCState.ScanCount & 0xff;
      break;

    case SPECIAL_REG_SURFACE_0_CURRENT_TRACK:
      FDCState.ResultReg = FDCState.CurrentTrack[0];
      break;

    case SPECIAL_REG_SURFACE_1_CURRENT_TRACK:
      FDCState.ResultReg = FDCState.CurrentTrack[1];
      break;

    case SPECIAL_REG_MODE_REGISTER:
      FDCState.ResultReg = FDCState.ModeReg;
      break;

    case SPECIAL_REG_DRIVE_CONTROL_OUTPUT_PORT:
      FDCState.ResultReg = FDCState.DriveControlOutputPort;
      break;

    case SPECIAL_REG_DRIVE_CONTROL_INPUT_PORT:
      FDCState.ResultReg = FDCState.DriveControlInputPort;
      break;

    case SPECIAL_REG_SURFACE_0_BAD_TRACK_1:
      FDCState.ResultReg = FDCState.BadTracks[0][0];
      break;

    case SPECIAL_REG_SURFACE_0_BAD_TRACK_2:
      FDCState.ResultReg = FDCState.BadTracks[0][1];
      break;

    case SPECIAL_REG_SURFACE_1_BAD_TRACK_1:
      FDCState.ResultReg = FDCState.BadTracks[1][0];
      break;

    case SPECIAL_REG_SURFACE_1_BAD_TRACK_2:
      FDCState.ResultReg = FDCState.BadTracks[1][1];
      break;

    default:
      #if ENABLE_LOG
      WriteLog("Read of bad special register\n");
      #endif
      return;
  }

  FDCState.StatusReg |= STATUS_REG_RESULT_FULL;
  UpdateNMIStatus();
}

/*--------------------------------------------------------------------------*/

static void DoBadCommand()
{
}

/*--------------------------------------------------------------------------*/

// The following table is used to parse commands from the command number
// written into the command register - it can't distinguish between subcommands
// selected from the first parameter.

typedef void (*CommandFunc)();

struct PrimaryCommandLookupType {
	unsigned char CommandNum;
	unsigned char Mask; // Mask command with this before comparing with CommandNum - allows drive ID to be removed
	int NParams; // Number of parameters to follow
	CommandFunc ToCall; // Called after all paameters have arrived
	CommandFunc IntHandler; // Called when interrupt requested by command is about to happen
	const char *Ident; // Mainly for debugging
};

static const PrimaryCommandLookupType PrimaryCommandLookup[] = {
	{ 0x00, 0x3f, 3, DoVarLength_ScanDataCommand,          nullptr,          "Scan Data (Variable Length/Multi-Record)" },
	{ 0x04, 0x3f, 3, DoVarLength_ScanDataAndDeldCommand,   nullptr,          "Scan Data & deleted data (Variable Length/Multi-Record)" },
	{ 0x0a, 0x3f, 2, Do128ByteSR_WriteDataCommand,         nullptr,          "Write Data (128 byte/single record)" },
	{ 0x0b, 0x3f, 3, DoVarLength_WriteDataCommand,         WriteInterrupt,   "Write Data (Variable Length/Multi-Record)" },
	{ 0x0e, 0x3f, 2, Do128ByteSR_WriteDeletedDataCommand,  nullptr,          "Write Deleted Data (128 byte/single record)" },
	{ 0x0f, 0x3f, 3, DoVarLength_WriteDeletedDataCommand,  nullptr,          "Write Deleted Data (Variable Length/Multi-Record)" },
	{ 0x12, 0x3f, 2, Do128ByteSR_ReadDataCommand,          nullptr,          "Read Data (128 byte/single record)" },
	{ 0x13, 0x3f, 3, DoVarLength_ReadDataCommand,          ReadInterrupt,    "Read Data (Variable Length/Multi-Record)" },
	{ 0x16, 0x3f, 2, Do128ByteSR_ReadDataAndDeldCommand,   Read128Interrupt, "Read Data & deleted data (128 byte/single record)" },
	{ 0x17, 0x3f, 3, DoVarLength_ReadDataAndDeldCommand,   ReadInterrupt,    "Read Data & deleted data (Variable Length/Multi-Record)" },
	{ 0x1b, 0x3f, 3, DoReadIDCommand,                      ReadIDInterrupt,  "ReadID" },
	{ 0x1e, 0x3f, 2, Do128ByteSR_VerifyDataAndDeldCommand, nullptr,          "Verify Data and Deleted Data (128 byte/single record)" },
	{ 0x1f, 0x3f, 3, DoVarLength_VerifyDataAndDeldCommand, VerifyInterrupt,  "Verify Data and Deleted Data (Variable Length/Multi-Record)" },
	{ 0x23, 0x3f, 5, DoFormatCommand,                      FormatInterrupt,  "Format" },
	{ 0x29, 0x3f, 1, DoSeekCommand,                        SeekInterrupt,    "Seek" },
	{ 0x2c, 0x3f, 0, DoReadDriveStatusCommand,             nullptr,          "Read drive status" },
	{ 0x35, 0xff, 4, DoSpecifyCommand,                     nullptr,          "Specify" },
	{ 0x3a, 0x3f, 2, DoWriteSpecialCommand,                nullptr,          "Write special registers" },
	{ 0x3d, 0x3f, 1, DoReadSpecialCommand,                 nullptr,          "Read special registers" },
	{ 0,    0,    0, DoBadCommand,                         nullptr,          "Unknown command" } // Terminator due to 0 mask matching all
};

/*--------------------------------------------------------------------------*/

// returns a pointer to the data structure for the given command
// If no matching command is given, the pointer points to an entry with a 0
// mask, with a sensible function to call.

static const PrimaryCommandLookupType *CommandPtrFromNumber(int CommandNumber)
{
	const PrimaryCommandLookupType *presptr = PrimaryCommandLookup;

	while (presptr->CommandNum != (presptr->Mask & CommandNumber))
	{
		presptr++;
	}

	return presptr;

	// FSD - could FSDPhysicalTrack = -1 here?
}

/*--------------------------------------------------------------------------*/

// Address is in the range 0-7 - with the fe80 etc stripped out

unsigned char Disc8271Read(int Address)
{
  unsigned char Value = 0;

  if (!Disc8271Enabled)
    return 0xFF;

  switch (Address) {
    case 0:
      #if ENABLE_LOG
      WriteLog("8271 Status register read (0x%0X)\n", StatusReg);
      #endif

      Value = FDCState.StatusReg;
      break;

    case 1:
      #if ENABLE_LOG
      WriteLog("8271 Result register read (0x%02X)\n", ResultReg);
      #endif

      // Clear interrupt request and result reg full flag
      FDCState.StatusReg &= ~(STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST);
      UpdateNMIStatus();
      Value = FDCState.ResultReg;
      FDCState.ResultReg = RESULT_REG_SUCCESS; // Register goes to 0 after its read
      break;

    case 4:
      #if ENABLE_LOG
      WriteLog("8271 data register read\n");
      #endif

      // Clear interrupt and non-dma request - not stated but DFS never looks at result reg!
      FDCState.StatusReg &= ~(STATUS_REG_INTERRUPT_REQUEST | STATUS_REG_NON_DMA_MODE);
      UpdateNMIStatus();
      Value = FDCState.DataReg;
      break;

    default:
      #if ENABLE_LOG
      WriteLog("8271: Read to unknown register address=%04X\n", Address);
      #endif
      break;
  }

  return Value;
}

/*--------------------------------------------------------------------------*/

static void CommandRegWrite(unsigned char Value)
{
  const PrimaryCommandLookupType *ptr = CommandPtrFromNumber(Value);

  #if ENABLE_LOG
  WriteLog("8271: Command register write value=0x%02X (Name=%s)\n", Value, ptr->Ident);
  #endif

  FDCState.Command = Value;
  FDCState.CommandParamCount = ptr->NParams;
  FDCState.CurrentParam = 0;

  FDCState.StatusReg |= STATUS_REG_COMMAND_BUSY | STATUS_REG_RESULT_FULL; // Observed on beeb for read special
  UpdateNMIStatus();

  // No parameters then call routine immediately
  if (FDCState.CommandParamCount == 0) {
    FDCState.StatusReg &= 0x7e;
    UpdateNMIStatus();
    ptr->ToCall();
  }
}

/*--------------------------------------------------------------------------*/

static void ParamRegWrite(unsigned char Value) {
  // Parameter wanted ?
  if (FDCState.CurrentParam >= FDCState.CommandParamCount) {
    #if ENABLE_LOG
    WriteLog("8271: Unwanted parameter register write value=0x%02X\n", Value);
    #endif
  } else {
    FDCState.Params[FDCState.CurrentParam++] = Value;

    FDCState.StatusReg &= 0xfe; // Observed on beeb
    UpdateNMIStatus();

    // Got all params yet?
    if (FDCState.CurrentParam >= FDCState.CommandParamCount) {

      FDCState.StatusReg &= 0x7e; // Observed on beeb
      UpdateNMIStatus();

      const PrimaryCommandLookupType *ptr = CommandPtrFromNumber(FDCState.Command);

      #if ENABLE_LOG
      WriteLog("<Disc access> 8271: All parameters arrived for '%s':", ptr->Ident);

      for (int i = 0; i < PresentParam; i++) {
        WriteLog(" %02X", Params[i]);
      }

      WriteLog("\n");
      #endif

      ptr->ToCall();
    }
  }
}

/*--------------------------------------------------------------------------*/

// Address is in the range 0-7 - with the fe80 etc stripped out

void Disc8271Write(int Address, unsigned char Value) {
  if (!Disc8271Enabled)
    return;

  // Clear a pending head unload
  if (DriveHeadUnloadPending) {
    DriveHeadUnloadPending = false;
    ClearTrigger(Disc8271Trigger);
  }

  switch (Address) {
    case 0:
      CommandRegWrite(Value);
      break;

    case 1:
      ParamRegWrite(Value);
      break;

    case 2:
      // The caller should write a 1 and then >11 cycles later a 0 - but I'm just going
      // to reset on both edges
      Disc8271Reset();
      break;

    case 4:
      FDCState.StatusReg &= ~(STATUS_REG_INTERRUPT_REQUEST | STATUS_REG_NON_DMA_MODE);
      UpdateNMIStatus();
      FDCState.DataReg = Value;
      break;

    default:
      break;
  }

  DriveHeadScheduleUnload();
}

/*--------------------------------------------------------------------------*/

static void DriveHeadScheduleUnload()
{
	// Schedule head unload when nothing else is pending.
	// This is mainly for the sound effects, but it also marks the drives as
	// not ready when the motor stops.
	if (DriveHeadLoaded && Disc8271Trigger == CycleCountTMax)
	{
		SetTrigger(4000000, Disc8271Trigger); // 2s delay to unload
		DriveHeadUnloadPending = true;
	}
}

/*--------------------------------------------------------------------------*/

static bool DriveHeadMotorUpdate()
{
	// This is mainly for the sound effects, but it also marks the drives as
	// not ready when the motor stops.
	int Drive = 0;

	if (DriveHeadUnloadPending)
	{
		// Mark drives as not ready
		FDCState.Select[0] = false;
		FDCState.Select[1] = false;
		DriveHeadUnloadPending = false;
		if (DriveHeadLoaded && DiscDriveSoundEnabled)
			PlaySoundSample(SAMPLE_HEAD_UNLOAD, false);
		DriveHeadLoaded = false;
		StopSoundSample(SAMPLE_DRIVE_MOTOR);
		StopSoundSample(SAMPLE_HEAD_SEEK);

		LEDs.Disc0 = false;
		LEDs.Disc1 = false;
		return true;
	}

	if (!DiscDriveSoundEnabled)
	{
		DriveHeadLoaded = true;
		return false;
	}

	if (!DriveHeadLoaded)
	{
		if (FDCState.Select[0]) LEDs.Disc0 = true;
		if (FDCState.Select[1]) LEDs.Disc1 = true;

		PlaySoundSample(SAMPLE_DRIVE_MOTOR, true);
		DriveHeadLoaded = true;
		PlaySoundSample(SAMPLE_HEAD_LOAD, false);
		SetTrigger(SAMPLE_HEAD_LOAD_CYCLES, Disc8271Trigger);
		return true;
	}

	if (FDCState.Select[0]) Drive = 0;
	if (FDCState.Select[1]) Drive = 1;

	StopSoundSample(SAMPLE_HEAD_SEEK);

	if (DriveHeadPosition[Drive] != FSDPhysicalTrack) // Internal_CurrentTrack[Drive]) {
	{
		int Tracks = abs(DriveHeadPosition[Drive] - FSDPhysicalTrack); // Internal_CurrentTrack[Drive]);

		if (Tracks > 1)
		{
			PlaySoundSample(SAMPLE_HEAD_SEEK, true);
			SetTrigger(Tracks * SAMPLE_HEAD_SEEK_CYCLES_PER_TRACK, Disc8271Trigger);
		}
		else
		{
			PlaySoundSample(SAMPLE_HEAD_STEP, false);
			SetTrigger(SAMPLE_HEAD_STEP_CYCLES, Disc8271Trigger);
		}

		if (DriveHeadPosition[Drive] < FSDPhysicalTrack) // Internal_CurrentTrack[Drive])
		{
			DriveHeadPosition[Drive] += Tracks;
		}
		else
		{
			DriveHeadPosition[Drive] -= Tracks;
		}

		return true;
	}

	return false;
}

/*--------------------------------------------------------------------------*/

void Disc8271_poll_real() {
  ClearTrigger(Disc8271Trigger);

  if (DriveHeadMotorUpdate())
    return;

  // Set the interrupt flag in the status register
  FDCState.StatusReg |= STATUS_REG_INTERRUPT_REQUEST;
  UpdateNMIStatus();

  if (CommandStatus.NextInterruptIsErr != RESULT_REG_DATA_CRC_ERROR &&
      CommandStatus.NextInterruptIsErr != RESULT_REG_DELETED_DATA_FOUND &&
      CommandStatus.NextInterruptIsErr != RESULT_REG_SUCCESS) {
    FDCState.ResultReg = CommandStatus.NextInterruptIsErr;
    FDCState.StatusReg = STATUS_REG_RESULT_FULL | STATUS_REG_INTERRUPT_REQUEST;
    UpdateNMIStatus();
    CommandStatus.NextInterruptIsErr = RESULT_REG_SUCCESS;
  } else {
    // Should only happen while a command is still active
    const PrimaryCommandLookupType *comptr = CommandPtrFromNumber(FDCState.Command);
    if (comptr->IntHandler != nullptr) comptr->IntHandler();
  }

  DriveHeadScheduleUnload();
}

/*--------------------------------------------------------------------------*/

// FSD - could be causing crashes, because of different sized tracks / sectors

void FreeDiscImage(int Drive)
{
  const int Head = 0;

  for (int Track = 0; Track < TRACKS_PER_DRIVE; Track++) {
    const int SectorsPerTrack = DiscStatus[Drive].Tracks[Head][Track].LogicalSectors;

    SectorType *SecPtr = DiscStatus[Drive].Tracks[Head][Track].Sectors;

    if (SecPtr != nullptr) {
      for (int Sector = 0; Sector < SectorsPerTrack; Sector++) {
        if (SecPtr[Sector].Data != nullptr) {
          free(SecPtr[Sector].Data);
          SecPtr[Sector].Data = nullptr;
        }
      }

      free(SecPtr);
      DiscStatus[Drive].Tracks[Head][Track].Sectors = nullptr;
    }
  }
}

/*--------------------------------------------------------------------------*/

void LoadSimpleDiscImage(const char *FileName, int DriveNum, int HeadNum, int Tracks) {
  FILE *infile=fopen(FileName,"rb");
  if (!infile) {
    mainWin->Report(MessageType::Error,
                    "Could not open disc file:\n  %s", FileName);

    return;
  }

  mainWin->SetImageName(FileName, DriveNum, DiscType::SSD);

  // JGH, 26-Dec-2011
  DiscStatus[DriveNum].NumHeads = 1; // 1 = TRACKS_PER_DRIVE SSD image
                                     // 2 = 2 * TRACKS_PER_DRIVE DSD image
  int Heads = 1;
  fseek(infile, 0L, SEEK_END);
  if (ftell(infile) > 0x40000) {
    Heads = 2; // Long sequential image continues onto side 1
    DiscStatus[DriveNum].NumHeads = 0; // 0 = 2 * TRACKS_PER_DRIVE SSD image
  }
  fseek(infile, 0L, SEEK_SET);
  // JGH

  strcpy(DiscStatus[DriveNum].FileName, FileName);
  DiscStatus[DriveNum].Type = DiscType::SSD;

  FreeDiscImage(DriveNum);

  for (int Head = HeadNum; Head < Heads; Head++)
  {
    for (int Track = 0; Track < Tracks; Track++)
    {
      DiscStatus[DriveNum].Tracks[Head][Track].LogicalSectors = 10;
      DiscStatus[DriveNum].Tracks[Head][Track].NSectors = 10;
      DiscStatus[DriveNum].Tracks[Head][Track].Gap1Size = 0; // Don't bother for the mo
      DiscStatus[DriveNum].Tracks[Head][Track].Gap3Size = 0;
      DiscStatus[DriveNum].Tracks[Head][Track].Gap5Size = 0;
      DiscStatus[DriveNum].Tracks[Head][Track].TrackIsReadable = true;
      SectorType *SecPtr = DiscStatus[DriveNum].Tracks[Head][Track].Sectors = (SectorType*)calloc(10, sizeof(SectorType));

      for (int Sector = 0; Sector < 10; Sector++) {
        SecPtr[Sector].IDField.LogicalTrack = Track; // was CylinderNum
        SecPtr[Sector].IDField.LogicalSector = Sector; // was RecordNum
        SecPtr[Sector].IDField.HeadNum = HeadNum;
        SecPtr[Sector].IDField.SectorLength = 256; // was PhysRecLength
        SecPtr[Sector].RecordNum = Sector;
        SecPtr[Sector].RealSectorSize = 256;
        SecPtr[Sector].Error = RESULT_REG_SUCCESS;
        SecPtr[Sector].Data = (unsigned char *)calloc(1,256);
        fread(SecPtr[Sector].Data, 1, 256, infile);
      }
    }
  }

  fclose(infile);
}

/*--------------------------------------------------------------------------*/

void LoadSimpleDSDiscImage(const char *FileName, int DriveNum, int Tracks) {
  FILE *infile=fopen(FileName,"rb");

  if (!infile) {
    mainWin->Report(MessageType::Error,
                    "Could not open disc file:\n  %s", FileName);

    return;
  }

  mainWin->SetImageName(FileName, DriveNum, DiscType::DSD);

  strcpy(DiscStatus[DriveNum].FileName, FileName);
  DiscStatus[DriveNum].Type = DiscType::DSD;

  DiscStatus[DriveNum].NumHeads = 2; // 2 = 2 * TRACKS_PER_DRIVE DSD image

  FreeDiscImage(DriveNum);

  for (int Track = 0; Track < Tracks; Track++)
  {
    for (int Head = 0; Head < 2; Head++)
    {
      DiscStatus[DriveNum].Tracks[Head][Track].LogicalSectors = 10;
      DiscStatus[DriveNum].Tracks[Head][Track].NSectors = 10;
      DiscStatus[DriveNum].Tracks[Head][Track].Gap1Size = 0; // Don't bother for the mo
      DiscStatus[DriveNum].Tracks[Head][Track].Gap3Size = 0;
      DiscStatus[DriveNum].Tracks[Head][Track].Gap5Size = 0;
      DiscStatus[DriveNum].Tracks[Head][Track].TrackIsReadable = true;
      SectorType *SecPtr = DiscStatus[DriveNum].Tracks[Head][Track].Sectors = (SectorType *)calloc(10,sizeof(SectorType));

      for (int Sector = 0; Sector < 10; Sector++) {
        SecPtr[Sector].IDField.LogicalTrack = Track; // was CylinderNum
        SecPtr[Sector].IDField.LogicalSector = Sector; // was RecordNum
        SecPtr[Sector].IDField.HeadNum = Head;
        SecPtr[Sector].IDField.SectorLength = 256; // was PhysRecLength
        SecPtr[Sector].RecordNum = Sector;
        SecPtr[Sector].RealSectorSize = 256;
        SecPtr[Sector].Error = RESULT_REG_SUCCESS;
        SecPtr[Sector].Data = (unsigned char *)calloc(1,256);
        fread(SecPtr[Sector].Data, 1, 256, infile);
      }
    }
  }

  fclose(infile);
}

/*--------------------------------------------------------------------------*/

static unsigned short GetFSDSectorSize(unsigned char Index)
{
	switch (Index)
	{
		case 0:
		default:
			return 128;

		case 1:
			return 256;

		case 2:
			return 512;

		case 3:
			return 1024;

		case 4:
			return 2048;
	}
}

/*--------------------------------------------------------------------------*/

void LoadFSDDiscImage(const char *FileName, int DriveNum) {
  FILE *infile = fopen(FileName,"rb");
  if (!infile) {
    mainWin->Report(MessageType::Error, "Could not open disc file:\n  %s", FileName);
    return;
  }

  mainWin->SetImageName(FileName, DriveNum, DiscType::FSD);

  // JGH, 26-Dec-2011
  DiscStatus[DriveNum].NumHeads = 1; // 1 = TRACKS_PER_DRIVE SSD image
                                     // 2 = 2 * TRACKS_PER_DRIVE DSD image
  const int Head = 0;

  strcpy(DiscStatus[DriveNum].FileName, FileName);
  DiscStatus[DriveNum].Type = DiscType::FSD;

  FreeDiscImage(DriveNum);

  unsigned char FSDheader[8]; // FSD - Header information
  fread(FSDheader, 1, 8, infile); // Skip FSD Header

  std::string disctitle;
  char dtchar = 1;

  while (dtchar != 0) {
    dtchar = fgetc(infile);
    disctitle = disctitle + dtchar;
  }

  int LastTrack = fgetc(infile) ; // Read number of last track on disk image
  DiscStatus[DriveNum].TotalTracks = LastTrack + 1;

  if (DiscStatus[DriveNum].TotalTracks > FSD_TRACKS_PER_DRIVE) {
    mainWin->Report(MessageType::Error,
                    "Could not open disc file:\n  %s\n\nExpected a maximum of %d tracks, found %d",
                    FileName, FSD_TRACKS_PER_DRIVE, DiscStatus[DriveNum].TotalTracks);

    return;
  }

  for (int CurrentTrack = 0; CurrentTrack < DiscStatus[DriveNum].TotalTracks; CurrentTrack++) {
    unsigned char fctrack = fgetc(infile); // Read current track details
    unsigned char SectorsPerTrack = fgetc(infile); // Read number of sectors on track
    DiscStatus[DriveNum].Tracks[Head][CurrentTrack].LogicalSectors = SectorsPerTrack;

    if (SectorsPerTrack > 0) // i.e., if the track is formatted
    {
      unsigned char TrackIsReadable = fgetc(infile); // Is track readable?
      DiscStatus[DriveNum].Tracks[Head][CurrentTrack].NSectors = SectorsPerTrack; // Can be different than 10
      SectorType *SecPtr = (SectorType*)calloc(SectorsPerTrack, sizeof(SectorType));
      DiscStatus[DriveNum].Tracks[Head][CurrentTrack].Sectors = SecPtr;
      DiscStatus[DriveNum].Tracks[Head][CurrentTrack].TrackIsReadable = TrackIsReadable != 0;

      for (int CurrentSector = 0; CurrentSector < SectorsPerTrack; CurrentSector++) {
        SecPtr[CurrentSector].CylinderNum = CurrentTrack;

        unsigned char LogicalTrack = fgetc(infile); // Logical track ID
        SecPtr[CurrentSector].IDField.LogicalTrack = LogicalTrack;

        unsigned char HeadNum = fgetc(infile); // Head number
        SecPtr[CurrentSector].IDField.HeadNum = HeadNum;

        unsigned char LogicalSector = fgetc(infile); // Logical sector ID
        SecPtr[CurrentSector].IDField.LogicalSector = LogicalSector;
        SecPtr[CurrentSector].RecordNum = CurrentSector;

        unsigned char FRecLength = fgetc(infile); // Reported length of sector
        SecPtr[CurrentSector].IDField.SectorLength = FRecLength;
        SecPtr[CurrentSector].IDSiz = GetFSDSectorSize(FRecLength);

        if (TrackIsReadable == 255) {
          unsigned char FPRecLength = fgetc(infile); // Real size of sector, can be misreported as copy protection
          unsigned short FSectorSize = GetFSDSectorSize(FPRecLength);

          SecPtr[CurrentSector].RealSectorSize = FSectorSize;

          unsigned char FErr = fgetc(infile); // Error code when sector was read
          SecPtr[CurrentSector].Error = FErr;
          SecPtr[CurrentSector].Data = (unsigned char *)calloc(1, FSectorSize);
          fread(SecPtr[CurrentSector].Data, 1, FSectorSize, infile);
        }
      }
    }
  }

  fclose(infile);
}

/*--------------------------------------------------------------------------*/

void Eject8271DiscImage(int DriveNum)
{
	strcpy(DiscStatus[DriveNum].FileName, "");
	FreeDiscImage(DriveNum);
}

/*--------------------------------------------------------------------------*/

static bool SaveTrackImage(int DriveNum, int HeadNum, int TrackNum) {
  bool Success = true;

  FILE *outfile = fopen(DiscStatus[DriveNum].FileName, "r+b");

  if (!outfile) {
    mainWin->Report(MessageType::Error,
                    "Could not open disc file for write:\n  %s", DiscStatus[DriveNum].FileName);

    return false;
  }

  long FileOffset;

  if (DiscStatus[DriveNum].NumHeads != 0) {
    FileOffset = (DiscStatus[DriveNum].NumHeads * TrackNum + HeadNum) * 2560; // 1=SSD, 2=DSD
  }
  else {
    FileOffset = (TrackNum + HeadNum * TRACKS_PER_DRIVE) * 2560; // 0=2-sided SSD
  }

  // Get the file length to check if the file needs extending
  long FileLength = 0;

  Success = fseek(outfile, 0L, SEEK_END) == 0;
  if (Success)
  {
    FileLength=ftell(outfile);
    if (FileLength == -1L) {
      Success = false;
    }
  }

  while (Success && FileOffset > FileLength)
  {
    if (fputc(0, outfile) == EOF)
      Success = false;
    FileLength++;
  }

  if (Success)
  {
    Success = fseek(outfile, FileOffset, SEEK_SET) == 0;

    SectorType *SecPtr = DiscStatus[DriveNum].Tracks[HeadNum][TrackNum].Sectors;

    for (int CurrentSector = 0; Success && CurrentSector < 10; CurrentSector++) {
      if (fwrite(SecPtr[CurrentSector].Data,1,256,outfile) != 256) {
        Success = false;
      }
    }
  }

  if (fclose(outfile) != 0) {
    Success = false;
  }

  if (!Success) {
    mainWin->Report(MessageType::Error,
                    "Failed writing to disc file:\n  %s", DiscStatus[DriveNum].FileName);
  }

  return Success;
}

/*--------------------------------------------------------------------------*/

bool IsDiscWritable(int DriveNum)
{
	return DiscStatus[DriveNum].Writeable;
}

/*--------------------------------------------------------------------------*/
void DiscWriteEnable(int DriveNum, bool WriteEnable) {
  bool DiscOK = true;

  DiscStatus[DriveNum].Writeable = WriteEnable;

  // If disc is being made writable then check that the disc catalogue will
  // not get corrupted if new files are added.  The files in the disc catalogue
  // must be in descending sector order otherwise the DFS ROMs write over
  // files at the start of the disc. The sector count in the catalogue must
  // also be correct.

  if (WriteEnable) {
    for (int HeadNum = 0; DiscOK && HeadNum < DiscStatus[DriveNum].NumHeads; HeadNum++) {
      SectorType *SecPtr = DiscStatus[DriveNum].Tracks[HeadNum][0].Sectors;
      if (SecPtr == nullptr)
        return; // No disc image!

      unsigned char *Data = SecPtr[1].Data;

      // Check for a Watford DFS 62 file catalogue
      int NumCatalogues = 2;
      Data = SecPtr[2].Data;
      for (int i = 0; i < 8; ++i)
        if (Data[i] != (unsigned char)0xaa) {
          NumCatalogues = 1;
          break;
        }

      for (int Catalogue = 0; DiscOK && Catalogue < NumCatalogues; ++Catalogue) {
        Data = SecPtr[Catalogue * 2 + 1].Data;

        // First check the number of sectors
        int NumSecs = ((Data[6] & 3) << 8) + Data[7];
        if (NumSecs != 0x320 && NumSecs != 0x190)
        {
          DiscOK = false;
        }
        else
        {
          // Now check the start sectors of each file
          int LastSec = 0x320;
          for (int File = 0; DiscOK && File < Data[5] / 8; ++File)
          {
            int StartSec = ((Data[File * 8 + 14] & 3) << 8) + Data[File * 8 + 15];
            if (LastSec < StartSec)
              DiscOK = false;
            LastSec = StartSec;
          }
        }
      }
    }

    if (!DiscOK)
    {
      mainWin->Report(MessageType::Error,
                      "WARNING - Invalid Disc Catalogue\n\n"
                      "This disc image will get corrupted if files are written to it.\n"
                      "Copy all the files to a new image to fix it.");
    }
  }
}

/*--------------------------------------------------------------------------*/

void Disc8271Reset() {
	static bool InitialInit = true;

	FDCState.ResultReg = RESULT_REG_SUCCESS;
	FDCState.StatusReg = 0;

	UpdateNMIStatus();

	FDCState.ScanSectorNum = 0;
	FDCState.ScanCount = 0; // Read as two bytes
	FDCState.ModeReg = 0;
	FDCState.CurrentTrack[0] = 0; // 0/1 for surface number
	FDCState.CurrentTrack[1] = 0;
	UsingSpecial = false; // FSD - Using special register
	FDCState.DriveControlOutputPort = 0;
	FDCState.DriveControlInputPort = 0;
	FDCState.BadTracks[0][0] = 0xff; // 1st subscript is surface 0/1 and second subscript is badtrack 0/1
	FDCState.BadTracks[0][1] = 0xff;
	FDCState.BadTracks[1][0] = 0xff;
	FDCState.BadTracks[1][1] = 0xff; 

	// Default values set by Acorn DFS:
	FDCState.StepRate = 12;
	FDCState.HeadSettlingTime = 10;
	FDCState.IndexCountBeforeHeadUnload = 12;
	FDCState.HeadLoadTime = 8;

	if (DriveHeadLoaded)
	{
		DriveHeadUnloadPending = true;
		DriveHeadMotorUpdate();
	}

	ClearTrigger(Disc8271Trigger); // No Disc8271Triggered events yet

	FDCState.Command = -1;
	FDCState.CommandParamCount = 0;
	FDCState.CurrentParam = 0;
	FDCState.Select[0] = false;
	FDCState.Select[1] = false;

	if (InitialInit)
	{
		InitialInit = false;
		InitDiscStore();
	}
}

/*--------------------------------------------------------------------------*/

void Save8271UEF(FILE *SUEF)
{
	char blank[256];
	memset(blank,0,256);

	fput16(0x046E,SUEF);
	fput32(613,SUEF);

	if (DiscStatus[0].Tracks[0][0].Sectors == nullptr) {
		// No disc in drive 0
		fwrite(blank,1,256,SUEF);
	}
	else {
		fwrite(DiscStatus[0].FileName, 1, 256, SUEF);
	}

	if (DiscStatus[1].Tracks[0][0].Sectors == nullptr) {
		// No disc in drive 1
		fwrite(blank,1,256,SUEF);
	}
	else {
		fwrite(DiscStatus[1].FileName, 1, 256, SUEF);
	}

	if (Disc8271Trigger == CycleCountTMax)
		fput32(Disc8271Trigger,SUEF);
	else
		fput32(Disc8271Trigger - TotalCycles,SUEF);
	fputc(FDCState.ResultReg,SUEF);
	fputc(FDCState.StatusReg,SUEF);
	fputc(FDCState.DataReg,SUEF);
	fputc(FDCState.ScanSectorNum, SUEF);
	fput32(FDCState.ScanCount, SUEF);
	fputc(FDCState.ModeReg, SUEF);
	fputc(FDCState.CurrentTrack[0], SUEF);
	fputc(FDCState.CurrentTrack[1], SUEF);
	fputc(FDCState.DriveControlOutputPort, SUEF);
	fputc(FDCState.DriveControlInputPort, SUEF);
	fputc(FDCState.BadTracks[0][0], SUEF);
	fputc(FDCState.BadTracks[0][1], SUEF);
	fputc(FDCState.BadTracks[1][0], SUEF);
	fputc(FDCState.BadTracks[1][1], SUEF);
	fput32(FDCState.Command,SUEF);
	fput32(FDCState.CommandParamCount, SUEF);
	fput32(FDCState.CurrentParam, SUEF);
	fwrite(FDCState.Params, 1, sizeof(FDCState.Params), SUEF);
	fput32(DiscStatus[0].NumHeads, SUEF);
	fput32(DiscStatus[1].NumHeads, SUEF);
	fput32(FDCState.Select[0] ? 1 : 0, SUEF);
	fput32(FDCState.Select[1] ? 1 : 0, SUEF);
	fput32(DiscStatus[0].Writeable ? 1 : 0, SUEF);
	fput32(DiscStatus[1].Writeable ? 1 : 0, SUEF);
	fput32(CommandStatus.FirstWriteInt ? 1 : 0, SUEF);
	fput32(CommandStatus.NextInterruptIsErr, SUEF);
	fput32(CommandStatus.TrackAddr,SUEF);
	fput32(CommandStatus.CurrentSector,SUEF);
	fput32(CommandStatus.SectorLength,SUEF);
	fput32(CommandStatus.SectorsToGo,SUEF);
	fput32(CommandStatus.ByteWithinSector,SUEF);
}

void Load8271UEF(FILE *SUEF)
{
	extern bool DiscLoaded[2];
	char FileName[256];
	char *ext;
	bool Loaded = false;
	bool LoadFailed = false;

	// Clear out current images, don't want them corrupted if
	// saved state was in middle of writing to disc.
	FreeDiscImage(0);
	FreeDiscImage(1);
	DiscLoaded[0] = false;
	DiscLoaded[1] = false;

	fread(FileName,1,256,SUEF);

	if (FileName[0]) {
		// Load drive 0
		Loaded = true;
		ext = strrchr(FileName, '.');
		if (ext != nullptr && _stricmp(ext+1, "dsd") == 0)
			LoadSimpleDSDiscImage(FileName, 0, 80);
		else
			LoadSimpleDiscImage(FileName, 0, 0, 80);

		if (DiscStatus[0].Tracks[0][0].Sectors == nullptr)
			LoadFailed = true;
	}

	fread(FileName,1,256,SUEF);

	if (FileName[0]) {
		// Load drive 1
		Loaded = true;
		ext = strrchr(FileName, '.');
		if (ext != nullptr && _stricmp(ext+1, "dsd") == 0)
			LoadSimpleDSDiscImage(FileName, 1, 80);
		else
			LoadSimpleDiscImage(FileName, 1, 0, 80);

		if (DiscStatus[1].Tracks[0][0].Sectors == nullptr)
			LoadFailed = true;
	}

	if (Loaded && !LoadFailed)
	{
		Disc8271Trigger=fget32(SUEF);
		if (Disc8271Trigger != CycleCountTMax)
			Disc8271Trigger+=TotalCycles;

		FDCState.ResultReg = fget8(SUEF);
		FDCState.StatusReg = fget8(SUEF);
		FDCState.DataReg = fget8(SUEF);
		FDCState.ScanSectorNum = fget8(SUEF);
		FDCState.ScanCount = fget32(SUEF);
		FDCState.ModeReg = fget8(SUEF);
		FDCState.CurrentTrack[0] = fget8(SUEF);
		FDCState.CurrentTrack[1] = fget8(SUEF);
		FDCState.DriveControlOutputPort = fget8(SUEF);
		FDCState.DriveControlInputPort = fget8(SUEF);
		FDCState.BadTracks[0][0] = fget8(SUEF);
		FDCState.BadTracks[0][1] = fget8(SUEF);
		FDCState.BadTracks[1][0] = fget8(SUEF);
		FDCState.BadTracks[1][1] = fget8(SUEF);
		FDCState.Command = fget32(SUEF);
		FDCState.CommandParamCount = fget32(SUEF);
		FDCState.CurrentParam = fget32(SUEF);
		fread(FDCState.Params, 1, sizeof(FDCState.Params), SUEF);
		DiscStatus[0].NumHeads = fget32(SUEF);
		DiscStatus[1].NumHeads = fget32(SUEF);
		FDCState.Select[0] = fget32(SUEF) != 0;
		FDCState.Select[1] = fget32(SUEF) != 0;
		DiscStatus[0].Writeable = fget32(SUEF) != 0;
		DiscStatus[1].Writeable = fget32(SUEF) != 0;
		CommandStatus.FirstWriteInt = fget32(SUEF) != 0;
		CommandStatus.NextInterruptIsErr = fget32(SUEF);
		CommandStatus.TrackAddr=fget32(SUEF);
		CommandStatus.CurrentSector=fget32(SUEF);
		CommandStatus.SectorLength=fget32(SUEF);
		CommandStatus.SectorsToGo=fget32(SUEF);
		CommandStatus.ByteWithinSector=fget32(SUEF);

		CommandStatus.CurrentTrackPtr=GetTrackPtr(CommandStatus.TrackAddr);

		if (CommandStatus.CurrentTrackPtr != nullptr)
		{
			CommandStatus.CurrentSectorPtr = GetSectorPtr(CommandStatus.CurrentTrackPtr,
			                                              CommandStatus.CurrentSector,
			                                              false);
		}
		else
		{
			CommandStatus.CurrentSectorPtr = nullptr;
		}
	}
}

/*--------------------------------------------------------------------------*/

void disc8271_dumpstate()
{
	WriteLog("8271:\n");
	WriteLog("  FDCState.ResultReg=%02X\n", FDCState.ResultReg);
	WriteLog("  FDCState.StatusReg=%02X\n", FDCState.StatusReg);
	WriteLog("  FDCState.DataReg=%02X\n", FDCState.DataReg);
	WriteLog("  FDCState.ScanSectorNum=%d\n", FDCState.ScanSectorNum);
	WriteLog("  FDCState.ScanCount=%u\n", FDCState.ScanCount);
	WriteLog("  FDCState.ModeReg=%02X\n", FDCState.ModeReg);
	WriteLog("  FDCState.CurrentTrack=%d, %d\n", FDCState.CurrentTrack[0],
	                                             FDCState.CurrentTrack[1]);
	WriteLog("  FDCState.DriveControlOutputPort=%02X\n", FDCState.DriveControlOutputPort);
	WriteLog("  FDCState.DriveControlInputPort=%02X\n", FDCState.DriveControlInputPort);
	WriteLog("  FDCState.BadTracks=(%d, %d) (%d, %d)\n", FDCState.BadTracks[0][0],
	                                                     FDCState.BadTracks[0][1],
	                                                     FDCState.BadTracks[1][0],
	                                                     FDCState.BadTracks[1][1]);
	WriteLog("  Disc8271Trigger=%d\n", Disc8271Trigger);
	WriteLog("  FDCState.Command=%d\n", FDCState.Command);
	WriteLog("  FDCState.CommandParamCount=%d\n", FDCState.CommandParamCount);
	WriteLog("  FDCState.CurrentParam=%d\n", FDCState.CurrentParam);
	WriteLog("  FDCState.Select=%d, %d\n", FDCState.Select[0], FDCState.Select[1]);
	WriteLog("  CommandStatus.NextInterruptIsErr=%02X\n", CommandStatus.NextInterruptIsErr);
}

/*--------------------------------------------------------------------------*/

void Get8271DiscInfo(int DriveNum, char *pFileName, int *Heads)
{
	strcpy(pFileName, DiscStatus[DriveNum].FileName);
	*Heads = DiscStatus[DriveNum].NumHeads;
}
