// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

//DL facts:
//	Ikaruga uses NO display lists!
//  Zelda WW uses TONS of display lists
//  Zelda TP uses almost 100% display lists except menus (we like this!)

// Note that it IS NOT POSSIBLE to precompile display lists! You can compile them as they are
// and hope that the vertex format doesn't change, though, if you do it just when they are 
// called. The reason is that the vertex format affects the sizes of the vertices.

#include "Globals.h"
#include "Profiler.h"
#include "OpcodeDecoding.h"
#include "VertexLoader.h"
#include "VertexShaderManager.h"
#include "TextureMngr.h"

#include "BPStructs.h"
#include "Fifo.h"
#include "DataReader.h"

#define CMDBUFFER_SIZE 1024*1024

#if ! defined(DATAREADER_INLINE) || defined(DATAREADER_DEBUG)
CDataReader_Fifo g_fifoReader;
#endif
#ifdef DATAREADER_DEBUG
u32 g_pDataReaderRealPtr=0;
#endif

#ifdef DATAREADER_INLINE
u32 g_pVideoData=0;
extern bool g_IsFifoRewinded;
#endif

void Decode();

#if !defined(DATAREADER_INLINE) || defined(DATAREADER_DEBUG)
extern u8 FAKE_PeekFifo8(u32 _uOffset);
extern u16 FAKE_PeekFifo16(u32 _uOffset);
extern u32 FAKE_PeekFifo32(u32 _uOffset);
extern int FAKE_GetFifoSize();
#endif
extern int FAKE_GetFifoEndAddr();
extern u32 FAKE_GetFifoStartPtr();
extern int FAKE_GetRealPtr();
extern void FAKE_SkipFifo(u32 skip);

template <class T>
void Xchg(T& a, T&b)
{
	T c = a;
	a = b;
	b = c;
}

void ExecuteDisplayList(u32 address, u32 size)
{
#if ! defined(DATAREADER_INLINE) || defined(DATAREADER_DEBUG)
    IDataReader* pOldReader = g_pDataReader;

    //address &= 0x01FFFFFF; // phys address
    CDataReader_Memory memoryReader(address);
    g_pDataReader = &memoryReader;
#endif
#ifdef DATAREADER_INLINE
	u32 old_pVideoData = g_pVideoData;

	const u64 startAddress = (u64)Memory_GetPtr(address);
	g_pVideoData = startAddress;
#endif
	// temporarily swap dl and non-dl(small "hack" for the stats)
	Xchg(stats.thisFrame.numDLPrims, stats.thisFrame.numPrims);
	Xchg(stats.thisFrame.numXFLoadsInDL, stats.thisFrame.numXFLoads);
	Xchg(stats.thisFrame.numCPLoadsInDL, stats.thisFrame.numCPLoads);
	Xchg(stats.thisFrame.numBPLoadsInDL, stats.thisFrame.numBPLoads);

#ifdef DATAREADER_INLINE
    while((g_pVideoData - startAddress) < size)
#else
    while((memoryReader.GetReadAddress() - address) < size)
#endif
    {
        Decode();
    }
    INCSTAT(stats.numDListsCalled);
    INCSTAT(stats.thisFrame.numDListsCalled);

	// un-swap
	Xchg(stats.thisFrame.numDLPrims, stats.thisFrame.numPrims);
	Xchg(stats.thisFrame.numXFLoadsInDL, stats.thisFrame.numXFLoads);
	Xchg(stats.thisFrame.numCPLoadsInDL, stats.thisFrame.numCPLoads);
	Xchg(stats.thisFrame.numBPLoadsInDL, stats.thisFrame.numBPLoads);

    // reset to the old reader
#ifdef DATAREADER_INLINE
	g_pVideoData = old_pVideoData;
#endif
#if defined(DATAREADER_DEBUG) || !defined(DATAREADER_INLINE)
    g_pDataReader = pOldReader;
#endif

}

bool FifoCommandRunnable(void)
{
#ifndef DATAREADER_INLINE
    u32 iBufferSize = FAKE_GetFifoSize();
#else
	u32 iBufferSize = FAKE_GetFifoEndAddr()-g_pVideoData;
#ifdef DATAREADER_DEBUG
    u32 iBufferSizedb = FAKE_GetFifoSize();
	if( iBufferSize != iBufferSizedb)	_asm int 3
#endif
#endif
    if (iBufferSize == 0)
        return false;

#if !defined(DATAREADER_INLINE)
	u8 Cmd = FAKE_PeekFifo8(0);	
#else
	u8 Cmd = DataPeek8(0);	
#ifdef DATAREADER_DEBUG
	if( Cmd != FAKE_PeekFifo8(0))	_asm int 3
#endif
#endif
    u32 iCommandSize = 0;

    switch(Cmd)
    {
    case GX_NOP:
        // Hm, this means that we scan over nop streams pretty slowly...
        iCommandSize = 1;
        break;

    case GX_LOAD_CP_REG:
        iCommandSize = 6;
        break;

    case GX_LOAD_INDX_A:
    case GX_LOAD_INDX_B:
    case GX_LOAD_INDX_C:
    case GX_LOAD_INDX_D:
        iCommandSize = 5;
        break;

    case GX_CMD_CALL_DL:	
        iCommandSize = 9;
        break;

    case 0x44:
        iCommandSize = 1;
        // zelda 4 swords calls it and checks the metrics registers after that
        break;

    case GX_CMD_INVL_VC: // invalid vertex cache - no parameter?
        iCommandSize = 1;
        break;

    case GX_LOAD_BP_REG:	
        iCommandSize = 5;
        break;

    case GX_LOAD_XF_REG:
        {
            // check if we can read the header
            if (iBufferSize >= 5)
			{				
                iCommandSize = 1 + 4;
#if !defined(DATAREADER_INLINE) || defined(DATAREADER_DEBUG)
                u32 Cmd2 = FAKE_PeekFifo32(1);
#ifdef DATAREADER_DEBUG
				if( Cmd2 != DataPeek32(1)) _asm int 3
#endif
#else
                u32 Cmd2 = DataPeek32(1);
#endif
                int dwTransferSize = ((Cmd2 >> 16) & 15) + 1;
                iCommandSize += dwTransferSize * 4;				
            }
            else
			{
                return false;
            }			
        }
        break;

    default:
        if (Cmd & 0x80)
        {				
            // check if we can read the header
            if (iBufferSize >= 3)
			{
                iCommandSize = 1 + 2;
#if !defined(DATAREADER_INLINE) || defined(DATAREADER_DEBUG)
                u16 numVertices = FAKE_PeekFifo16(1);
#ifdef DATAREADER_DEBUG
				if( numVertices != DataPeek16(1))	_asm int 3
#endif
#else
                u16 numVertices = DataPeek16(1);
#endif
                VertexLoader& vtxLoader = g_VertexLoaders[Cmd & GX_VAT_MASK];
                iCommandSize += numVertices * vtxLoader.ComputeVertexSize();
            }
            else {				
                return false;
            }
        }
        else {
            char szTemp[1024];
            sprintf(szTemp, "GFX: Unknown Opcode (0x%x).\n"
				"This means one of the following:\n"
				"* The emulated GPU got desynced, disabling dual core can help\n"
				"* Command stream corrupted by some spurious memory bug\n"
				"* This really is an unknown opcode (unlikely)\n"
				"* Some other sort of bug\n\n"
				"Dolphin will now likely crash or hang. Enjoy.", Cmd);
            g_VideoInitialize.pSysMessage(szTemp);
            g_VideoInitialize.pLog(szTemp, TRUE);
        }
        break;
    }
    
    if (iCommandSize > iBufferSize)
        return false;

    // INFO_LOG("OP detected: Cmd 0x%x  size %i  buffer %i",Cmd, iCommandSize, iBufferSize);

    return true;
}

void Decode(void)
{
    int Cmd = DataReadU8();
    switch(Cmd)
    {
    case GX_NOP:
        break;

    case GX_LOAD_CP_REG: //0x08
        {
            u32 SubCmd = DataReadU8();
            u32 Value = DataReadU32();
            VertexManager::LoadCPReg(SubCmd,Value);
			INCSTAT(stats.thisFrame.numCPLoads);
        }
        break;

    case GX_LOAD_XF_REG:
        {
            u32 Cmd2 = DataReadU32();

            int dwTransferSize = ((Cmd2>>16)&15) + 1;
            u32 dwAddress = Cmd2 & 0xFFFF;
            static u32 pData[16];
            for (int i=0; i<dwTransferSize; i++)
                pData[i] = DataReadU32();
            VertexShaderMngr::LoadXFReg(dwTransferSize,dwAddress,pData);
			INCSTAT(stats.thisFrame.numXFLoads);
        }
        break;

    case GX_LOAD_INDX_A: //used for position matrices
        VertexShaderMngr::LoadIndexedXF(DataReadU32(),0xC);
        break;
    case GX_LOAD_INDX_B: //used for normal matrices
        VertexShaderMngr::LoadIndexedXF(DataReadU32(),0xD);
        break;
    case GX_LOAD_INDX_C: //used for postmatrices
        VertexShaderMngr::LoadIndexedXF(DataReadU32(),0xE);
        break;
    case GX_LOAD_INDX_D: //used for lights
        VertexShaderMngr::LoadIndexedXF(DataReadU32(),0xF);
        break;

    case GX_CMD_CALL_DL:
        {
            u32 dwAddr  = DataReadU32();
            u32 dwCount = DataReadU32();
            ExecuteDisplayList(dwAddr, dwCount);
        }			
        break;

    case 0x44:
        // zelda 4 swords calls it and checks the metrics registers after that
        break;

    case GX_CMD_INVL_VC:// Invalidate	(vertex cache?)	
        DebugLog("Invalidate	(vertex cache?)");
        break;

    case GX_LOAD_BP_REG: //0x61
        {
			u32 cmd = DataReadU32();
            LoadBPReg(cmd);
			INCSTAT(stats.thisFrame.numBPLoads);
        }
        break;

    // draw primitives 
    default:
        if (Cmd&0x80)
        {
            // load vertices (use computed vertex size from FifoCommandRunnable above)
			u16 numVertices = DataReadU16();
            if (numVertices > 0) {
                g_VertexLoaders[Cmd & GX_VAT_MASK].RunVertices((Cmd & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT, numVertices);
            }
        }
        else
        {
            // char szTmp[256];
            //sprintf(szTmp, "Illegal command %02x (at %08x)",Cmd,g_pDataReader->GetPtr());
            //g_VideoInitialize.pLog(szTmp);
            //MessageBox(0,szTmp,"GFX ERROR",0);
            // _assert_msg_(0,szTmp,"");
            break;
        }
        break;
    }
}

void OpcodeDecoder_Init()
{	
#if !defined(DATAREADER_INLINE)
    g_pDataReader = &g_fifoReader;
#else
	g_pVideoData = FAKE_GetFifoStartPtr();
#if defined(DATAREADER_DEBUG)
    g_pDataReader = &g_fifoReader;
	g_pDataReaderRealPtr = g_pDataReader->GetRealPtr();
	DATAREADER_DEBUG_CHECK_PTR;
#endif
#endif
}


void OpcodeDecoder_Shutdown()
{
}

void OpcodeDecoder_Run()
{
    DVSTARTPROFILE();
    while (FifoCommandRunnable())
    {
		DATAREADER_DEBUG_CHECK_PTR;
        Decode();
    }
}
