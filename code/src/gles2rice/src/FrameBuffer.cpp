/*
Copyright (C) 2005 Rice1964

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// ===========================================================================

#include <vector>

#include <retro_miscellaneous.h>

#include "ConvertImage.h"
#include "DeviceBuilder.h"
#include "FrameBuffer.h"
#include "UcodeDefs.h"
#include "RSP_Parser.h"
#include "Render.h"

#include "../../Graphics/RSP/RSP_state.h"

extern TMEMLoadMapInfo g_tmemLoadAddrMap[0x200];    // Totally 4KB TMEM;

// 0 keeps the most recent CI info
// 1 keeps the frame buffer CI info which is being displayed now
// 2 keeps the older frame buffer CI info. This can be used if we are using triple buffer
/* Overview of framebuffer implementation
1) Check if backbuffer has changed, via different detection techniques
2) If changed, we copy the GFX card's backbuffer to main RAM
3) This is slow due to the reading process, not the writing
*/

RecentCIInfo g_RecentCIInfo[5];
RecentCIInfo *g_uRecentCIInfoPtrs[5] =
{
    &g_RecentCIInfo[0],
    &g_RecentCIInfo[1],
    &g_RecentCIInfo[2],
    &g_RecentCIInfo[3],
    &g_RecentCIInfo[4],
};

int numOfRecentCIInfos = 5;

RecentViOriginInfo g_RecentVIOriginInfo[5];
uint32_t dwBackBufferSavedAtFrame=0;

RenderTextureInfo gRenderTextureInfos[20];
int numOfTxtBufInfos = sizeof(gRenderTextureInfos)/sizeof(RenderTextureInfo);
RenderTextureInfo *g_pRenderTextureInfo = NULL;

FrameBufferManager* g_pFrameBufferManager = NULL;

bool LastCIIsNewCI=false;

FrameBufferManager::FrameBufferManager() :
    m_isRenderingToTexture(false),
    m_curRenderTextureIndex(-1),
        m_lastTextureBufferIndex(-1)
{
}

FrameBufferManager::~FrameBufferManager()
{
}

void FrameBufferManager::CloseUp()
{
   unsigned i;

   for (i=0; i<numOfTxtBufInfos; i++)
   {
      if (gRenderTextureInfos[i].pRenderTexture)
         free(gRenderTextureInfos[i].pRenderTexture);
      gRenderTextureInfos[i].pRenderTexture = NULL;
   }
}

void FrameBufferManager::Initialize()
{
    m_isRenderingToTexture = false;
    m_lastTextureBufferIndex = -1;
    m_curRenderTextureIndex = -1;
    
    status.bCIBufferIsRendered = false;
    status.bN64IsDrawingTextureBuffer = false;
    status.bHandleN64RenderTexture = false;
    status.bN64FrameBufferIsUsed = false;

    memset(&gRenderTextureInfos[0], 0, sizeof(RenderTextureInfo)*numOfTxtBufInfos);
}
// ===========================================================================

uint16_t ConvertRGBATo555(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t ar = a>=0x20?1:0;
    return ((r>>3)<<RGBA5551_RedShift) | ((g>>3)<<RGBA5551_GreenShift) | ((b>>3)<<RGBA5551_BlueShift) | ar;//(a>>7);
}

uint16_t ConvertRGBATo555(uint32_t color32)
{
    return (uint16_t)((((color32>>19)&0x1F)<<RGBA5551_RedShift) | (((color32>>11)&0x1F)<<RGBA5551_GreenShift) | (((color32>>3)&0x1F)<<RGBA5551_BlueShift) | ((color32>>31)));;
}

void FrameBufferManager::UpdateRecentCIAddr(SetImgInfo &ciinfo)
{
    if (ciinfo.dwAddr == g_uRecentCIInfoPtrs[0]->dwAddr)
        return;

    RecentCIInfo *temp = NULL;

    int i;
    for( i=1; i<numOfRecentCIInfos; i++ )
    {
        if (ciinfo.dwAddr == g_uRecentCIInfoPtrs[i]->dwAddr)
        {
            temp = g_uRecentCIInfoPtrs[i];

            for (int j=i; j>0; j--)
            {
                g_uRecentCIInfoPtrs[j] = g_uRecentCIInfoPtrs[j-1];
            }
            break;
        }
    }

    if (i >= numOfRecentCIInfos)
    {
        temp = g_uRecentCIInfoPtrs[4];
        g_uRecentCIInfoPtrs[4] = g_uRecentCIInfoPtrs[3];
        g_uRecentCIInfoPtrs[3] = g_uRecentCIInfoPtrs[2];
        g_uRecentCIInfoPtrs[2] = g_uRecentCIInfoPtrs[1];
        g_uRecentCIInfoPtrs[1] = g_uRecentCIInfoPtrs[0];
        temp->dwCopiedAtFrame = 0;
        temp->bCopied = false;
    }

    g_uRecentCIInfoPtrs[0] = temp;

    // Fix me here for Mario Tennis
    temp->dwLastWidth = windowSetting.uViWidth;
    temp->dwLastHeight = windowSetting.uViHeight;

    temp->dwFormat = ciinfo.dwFormat;
    temp->dwAddr = ciinfo.dwAddr;
    temp->dwSize = ciinfo.dwSize;
    temp->dwWidth = ciinfo.dwWidth;
    temp->dwHeight = gRDP.scissor.bottom;
    temp->dwMemSize = (temp->dwWidth*temp->dwHeight/2)<<temp->dwSize;
    temp->bCopied = false;
    temp->lastUsedFrame = status.gDlistCount;
    temp->lastSetAtUcode = status.gUcodeCount;
}


/************************************************************************/
/* Mark the ciinfo entry that the ciinfo is used by VI origin register  */
/* in another word, this is a real frame buffer, not a fake frame buffer*/
/* Fake frame buffers are never really used by VI origin                */
/************************************************************************/
void FrameBufferManager::SetAddrBeDisplayed(uint32_t addr)
{
    uint32_t viwidth = *gfx_info.VI_WIDTH_REG;
    addr &= (g_dwRamSize-1);

    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if (g_uRecentCIInfoPtrs[i]->dwAddr+2*viwidth == addr)
        {
            g_uRecentCIInfoPtrs[i]->bUsedByVIAtFrame = status.gDlistCount;
        }
        else if (addr >= g_uRecentCIInfoPtrs[i]->dwAddr && addr < g_uRecentCIInfoPtrs[i]->dwAddr+0x1000)
        {
            g_uRecentCIInfoPtrs[i]->bUsedByVIAtFrame = status.gDlistCount;
        }
    }

    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if (g_RecentVIOriginInfo[i].addr == addr)
        {
            g_RecentVIOriginInfo[i].FrameCount = status.gDlistCount;
            return;
        }
    }

    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if (g_RecentVIOriginInfo[i].addr == 0)
        {
            // Never used
            g_RecentVIOriginInfo[i].addr = addr;
            g_RecentVIOriginInfo[i].FrameCount = status.gDlistCount;
            return;
        }
    }

    int index=0;
    uint32_t minFrameCount = 0xffffffff;

    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if( g_RecentVIOriginInfo[i].FrameCount < minFrameCount )
        {
            index = i;
            minFrameCount = g_RecentVIOriginInfo[i].FrameCount;
        }
    }

    g_RecentVIOriginInfo[index].addr = addr;
    g_RecentVIOriginInfo[index].FrameCount = status.gDlistCount;
}

bool FrameBufferManager::HasAddrBeenDisplayed(uint32_t addr, uint32_t width)
{
    addr &= (g_dwRamSize-1);

    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if (g_uRecentCIInfoPtrs[i]->dwAddr == 0)
            continue;

        if (g_uRecentCIInfoPtrs[i]->dwAddr == addr)
        {
            if (status.gDlistCount-g_uRecentCIInfoPtrs[i]->bUsedByVIAtFrame < 20)
                //if (g_uRecentCIInfoPtrs[i]->bUsedByVIAtFrame != 0)
            {
                return true;
            }
            else
            {
                TXTRBUF_DUMP(TRACE0("This is a new buffer address, the address is never a displayed buffer"););
                return false;
            }
        }
    }

    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if (g_RecentVIOriginInfo[i].addr != 0)
        {
            if (g_RecentVIOriginInfo[i].addr > addr && 
                (g_RecentVIOriginInfo[i].addr - addr)%width == 0 &&
                (g_RecentVIOriginInfo[i].addr - addr)/width <= 4)
            {
                if (status.gDlistCount-g_RecentVIOriginInfo[i].FrameCount < 20)
                    //if (g_RecentVIOriginInfo[i].FrameCount != 0)
                {
                    return true;
                }
                else
                {
                    TXTRBUF_DUMP(DebuggerAppendMsg("This is a new buffer address, the address is never a displayed buffer"););
                    return false;
                }
            }
        }
    }

    if (status.gDlistCount > 20)
    {
        return false;
    }
    else
    {
        TXTRBUF_DUMP({DebuggerAppendMsg("This is a new buffer address, the address is never a displayed buffer");});
        return true;
    }
}

int FrameBufferManager::FindRecentCIInfoIndex(uint32_t addr)
{
    for (int i=0; i<numOfRecentCIInfos; i++)
    {
        if (g_uRecentCIInfoPtrs[i]->dwAddr <= addr && addr < g_uRecentCIInfoPtrs[i]->dwAddr+g_uRecentCIInfoPtrs[i]->dwMemSize)
        {
            return i;
        }
    }

    return -1;
}

bool FrameBufferManager::IsDIaRenderTexture()
{
    bool foundSetScissor=false;
    bool foundFillRect=false;
    bool foundSetFillColor=false;
    bool foundSetCImg=false;
    uint32_t newFillColor = 0;

    uint32_t dwPC = __RSP.PC[__RSP.PCi];       // This points to the next instruction

    for (int i=0; i<10; i++)
    {
       uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
        uint32_t w0 = *(uint32_t *)(rdram_u8 + dwPC + i*8);
        uint32_t w1 = *(uint32_t *)(rdram_u8 + dwPC + 4 + i*8);

        if ((w0>>24) == G_SETSCISSOR)
        {
            foundSetScissor = true;
            continue;
        }

        if ((w0>>24) == G_SETFILLCOLOR)
        {
            foundSetFillColor = true;
            newFillColor = w1;
            continue;
        }

        if ((w0>>24) == G_FILLRECT)
        {
            uint32_t x0   = ((w1>>12)&0xFFF)/4;
            uint32_t y0   = ((w1>>0 )&0xFFF)/4;
            uint32_t x1   = ((w0>>12)&0xFFF)/4;

            if (x0 == 0 && y0 == 0)
            {
                if (x1 == g_CI.dwWidth)
                {
                    foundFillRect = true;
                    continue;
                }

                if (x1 == (unsigned int)(g_CI.dwWidth-1))
                {
                    foundFillRect = true;
                    continue;
                }
            }
        }   

        if ((w0>>24) == G_TEXRECT)
        {
            break;
        }

        if ((w0>>24) == G_SETCIMG)
        {
            foundSetCImg = true;
            break;
        }
    }

    /*
    bool foundSetScissor=false;
    bool foundFillRect=false;
    bool foundSetFillColor=false;
    bool foundSetCImg=false;
    bool foundTxtRect=false;
    int ucodeLength=10;
    uint32_t newFillColor;
    */

    if (foundFillRect)
    {
        if (foundSetFillColor)
        {
            if (newFillColor != 0xFFFCFFFC)
                return true;    // this is a render_texture
            else
                return false;
        }

        if (gRDP.fillColor != 0x00FFFFF7)
            return true;    // this is a render_texture
        else
            return false;   // this is a normal ZImg
    }
    else if (foundSetFillColor && newFillColor == 0xFFFCFFFC && foundSetCImg)
    {
        return false;
    }
    else
    {
        return true;
    }


    if (!foundSetCImg)
        return true;

    if (foundSetScissor)
        return true;
}

int FrameBufferManager::CheckAddrInBackBuffers(uint32_t addr, uint32_t memsize, bool copyToRDRAM)
{
    int r = FindRecentCIInfoIndex(addr);

    if (r >= 0)
    {
        // Also check if the address is overwritten by a recent render_texture
        //int t = CheckAddrInRenderTextures(addr, false);
        int t = -1;
        for (int i=0; i<numOfTxtBufInfos; i++)
        {
            uint32_t bufHeight = gRenderTextureInfos[i].knownHeight ? gRenderTextureInfos[i].N64Height : gRenderTextureInfos[i].maxUsedHeight;
            uint32_t bufMemSize = gRenderTextureInfos[i].CI_Info.dwSize*gRenderTextureInfos[i].N64Width*bufHeight;
            if (addr >=gRenderTextureInfos[i].CI_Info.dwAddr && addr < gRenderTextureInfos[i].CI_Info.dwAddr+bufMemSize)
            {
                if (g_uRecentCIInfoPtrs[r]->lastSetAtUcode < gRenderTextureInfos[i].updateAtUcodeCount)
                {
                    t = i;
                    break;
                }
            }
        }

        if (t >= 0)
            return -1;
    }

    if (r >= 0 && status.gDlistCount - g_uRecentCIInfoPtrs[r]->lastUsedFrame <= 3  && g_uRecentCIInfoPtrs[r]->bCopied == false)
        SaveBackBuffer(r, NULL, true);

    return r;
}


uint8_t CIFindIndex(uint16_t val)
{
    for (int i=0; i<=0xFF; i++)
    {
       if (val == g_wRDPTlut[i])
          return (uint8_t)i;
    }

    return 0;
}


void TexRectToFrameBuffer_8b(uint32_t dwXL, uint32_t dwYL, uint32_t dwXH, uint32_t dwYH, float t0u0, float t0v0, float t0u1, float t0v1, uint32_t dwTile)
{
    // Copy the framebuffer texture into the N64 framebuffer memory
    // Used in Yoshi

    uint32_t maxW = g_pRenderTextureInfo->N64Width;
    uint32_t maxH = g_pRenderTextureInfo->N64Height;

    uint32_t maxOff = maxW*maxH;

    TMEMLoadMapInfo &info = g_tmemLoadAddrMap[gDP.tiles[dwTile].tmem];
    uint32_t dwWidth = dwXH-dwXL;
    uint32_t dwHeight = dwYH-dwYL;

    float xScale = (t0u1-t0u0)/dwWidth;
    float yScale = (t0v1-t0v0)/dwHeight;

    uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
    uint8_t* dwSrc = rdram_u8 + info.dwLoadAddress;
    uint8_t* dwDst = rdram_u8 + g_pRenderTextureInfo->CI_Info.dwAddr;

    uint32_t dwSrcPitch = gRDP.tilesinfo[dwTile].dwPitch;
    uint32_t dwDstPitch = g_pRenderTextureInfo->CI_Info.dwWidth;

    uint32_t dwSrcOffX = gRDP.tilesinfo[dwTile].hilite_sl;
    uint32_t dwSrcOffY = gRDP.tilesinfo[dwTile].hilite_tl;

    uint32_t dwLeft = dwXL;
    uint32_t dwTop = dwYL;

    dwWidth = MIN(dwWidth, maxW-dwLeft);
    dwHeight = MIN(dwHeight, maxH-dwTop);
    
    if (maxH <= dwTop)
        return;

    for (uint32_t y = 0; y < dwHeight; y++)
    {
        uint32_t dwByteOffset = (uint32_t)(((y*yScale+dwSrcOffY) * dwSrcPitch) + dwSrcOffX);

        for (uint32_t x = 0; x < dwWidth; x++)
        {
            if ((((y+dwTop)*dwDstPitch+x+dwLeft)^0x3) > maxOff)
            {
#ifdef DEBUGGER
                TRACE0("Warning: Offset exceeds limit");
#endif
                continue;
            }
            dwDst[((y+dwTop)*dwDstPitch+x+dwLeft)^0x3] = dwSrc[(uint32_t)(dwByteOffset+x*xScale) ^ 0x3];
        }
    }

    TXTRBUF_DUMP(DebuggerAppendMsg("TexRect To FrameBuffer: X0=%d, Y0=%d, X1=%d, Y1=%d,\n\t\tfS0=%f, fT0=%f, fS1=%f, fT1=%f ",
        dwXL, dwYL, dwXH, dwYH, t0v0, t0v0, t0u1, t0v1););
}

void TexRectToN64FrameBuffer_16b(uint32_t x0, uint32_t y0, uint32_t width, uint32_t height, uint32_t dwTile)
{
    // Copy the framebuffer texture into the N64 RDRAM framebuffer memory structure

    DrawInfo srcInfo;   
    if (g_textures[dwTile].m_pCTexture->StartUpdate(&srcInfo) == false)
    {
        DebuggerAppendMsg("Fail to lock texture:TexRectToN64FrameBuffer_16b");
        return;
    }

    uint32_t n64CIaddr = g_CI.dwAddr;
    uint32_t n64CIwidth = g_CI.dwWidth;

    for (uint32_t y = 0; y < height; y++)
    {
       uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
        uint32_t* pSrc = (uint32_t*)((uint8_t*)srcInfo.lpSurface + y * srcInfo.lPitch);
        uint16_t* pN64Buffer = (uint16_t*)(rdram_u8 + (n64CIaddr&(g_dwRamSize-1)))+(y+y0)*n64CIwidth;

        for (uint32_t x = 0; x < width; x++)
        {
            pN64Buffer[x+x0] = ConvertRGBATo555(pSrc[x]);
        }
    }

    g_textures[dwTile].m_pCTexture->EndUpdate(&srcInfo);
}

#define FAST_CRC_CHECKING_INC_X 13
#define FAST_CRC_CHECKING_INC_Y 11
#define FAST_CRC_MIN_Y_INC      2
#define FAST_CRC_MIN_X_INC      2
#define FAST_CRC_MAX_X_INC      7
#define FAST_CRC_MAX_Y_INC      3
extern uint32_t dwAsmHeight;
extern uint32_t dwAsmPitch;
extern uint32_t dwAsmdwBytesPerLine;
extern uint32_t dwAsmCRC;
extern uint8_t* pAsmStart;

uint32_t CalculateRDRAMCRC(void *pPhysicalAddress, uint32_t left, uint32_t top, uint32_t width, uint32_t height, uint32_t size, uint32_t pitchInBytes )
{
    dwAsmCRC = 0;
    dwAsmdwBytesPerLine = ((width<<size)+1)/2;

    if (currentRomOptions.bFastTexCRC && !options.bLoadHiResTextures && (height>=32 || (dwAsmdwBytesPerLine>>2)>=16))
    {
        uint32_t realWidthInDWORD = dwAsmdwBytesPerLine>>2;
        uint32_t xinc = realWidthInDWORD / FAST_CRC_CHECKING_INC_X;   
        if (xinc < FAST_CRC_MIN_X_INC)
            xinc = MIN(FAST_CRC_MIN_X_INC, width);
        if (xinc > FAST_CRC_MAX_X_INC)
            xinc = FAST_CRC_MAX_X_INC;

        uint32_t yinc = height / FAST_CRC_CHECKING_INC_Y; 
        if (yinc < FAST_CRC_MIN_Y_INC) 
            yinc = MIN(FAST_CRC_MIN_Y_INC, height);
        if (yinc > FAST_CRC_MAX_Y_INC)
            yinc = FAST_CRC_MAX_Y_INC;

        uint32_t pitch   = pitchInBytes>>2;
        uint32_t *pStart = (uint32_t*)(pPhysicalAddress);
        pStart += (top * pitch) + (((left<<size)+1)>>3);

        // The original assembly code had a bug in it (it incremented pStart by 'pitch' in bytes, not in dwords)
        // This C code implements the same algorithm as the ASM but without the bug
        uint32_t y = 0;
        while (y < height)
        {
            uint32_t x = 0;
            while (x < realWidthInDWORD)
            {
                dwAsmCRC = (dwAsmCRC << 4) + ((dwAsmCRC >> 28) & 15);
                dwAsmCRC += pStart[x];
                x += xinc;
                dwAsmCRC += x;
            }
            dwAsmCRC ^= y;
            y += yinc;
            pStart += pitch;
        }
    }
    else
    {
       dwAsmdwBytesPerLine = ((width<<size)+1)/2;

       pAsmStart = (uint8_t*)(pPhysicalAddress);
       pAsmStart += (top * pitchInBytes) + (((left<<size)+1)>>1);

       dwAsmHeight = height - 1;
       dwAsmPitch = pitchInBytes;

       uint32_t pitch = pitchInBytes>>2;
       uint32_t* pStart = (uint32_t*)pPhysicalAddress;
       pStart += (top * pitch) + (((left<<size)+1)>>3);

       int y = dwAsmHeight;

       while (y >= 0)
       {
          uint32_t esi = 0;
          int x = dwAsmdwBytesPerLine - 4;
          while (x >= 0)
          {
             esi = *(uint32_t*)(pAsmStart + x);
             esi ^= x;

             dwAsmCRC = (dwAsmCRC << 4) + ((dwAsmCRC >> 28) & 15);
             dwAsmCRC += esi;
             x-=4;
          }
          esi ^= y;
          dwAsmCRC += esi;
          pAsmStart += dwAsmPitch;
          y--;
       }

    }
    return dwAsmCRC;
}
unsigned char CalculateMaxCI(void *pPhysicalAddress, uint32_t left, uint32_t top, uint32_t width, uint32_t height, uint32_t size, uint32_t pitchInBytes )
{
    unsigned char *buf;
    unsigned char val = 0;

    if (G_IM_SIZ_8b == size)
    {
        for (uint32_t y = 0; y<height; y++)
        {
            buf = (unsigned char*)pPhysicalAddress + left + pitchInBytes * (y+top);
            for (uint32_t x=0; x<width; x++)
            {
                if (buf[x] > val)
                    val = buf[x];

                if (val == 0xFF)
                    return 0xFF;
            }
        }
    }
    else
    {
        unsigned char val1,val2;
        left >>= 1;
        width >>= 1;
        for (uint32_t y = 0; y<height; y++)
        {
            buf = (unsigned char*)pPhysicalAddress + left + pitchInBytes * (y+top);
            for (uint32_t x=0; x<width; x++)
            {
                val1 = buf[x]>>4;
                val2 = buf[x]&0xF;

                if (val1 > val) val = val1;
                if (val2 > val) val = val2;

                if (val == 0xF)
                    return 0xF;
            }
        }
    }

    return val;
}

bool FrameBufferManager::FrameBufferInRDRAMCheckCRC()
{
   uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
    RecentCIInfo &p = *(g_uRecentCIInfoPtrs[0]);
    uint8_t *pFrameBufferBase = (uint8_t*)(rdram_u8 + p.dwAddr);
    uint32_t pitch = (p.dwWidth << p.dwSize ) >> 1;
    uint32_t crc = CalculateRDRAMCRC(pFrameBufferBase, 0, 0, p.dwWidth, p.dwHeight, p.dwSize, pitch);
    if (crc != p.dwCRC)
    {
        p.dwCRC = crc;
        TRACE0("Frame Buffer CRC mismatch, it was modified by the CPU");
        return false;
    }
    else
    {
        return true;
    }
}

extern std::vector<uint32_t> frameWriteRecord;
void FrameBufferManager::FrameBufferWriteByCPU(uint32_t addr, uint32_t size)
{
    if (!frameBufferOptions.bProcessCPUWrite)
        return;

    //WARNING(TRACE2("Frame Buffer Write, address=%08X, CI Address=%08X", addr, g_CI.dwAddr));
    status.frameWriteByCPU = true;
    frameWriteRecord.push_back(addr&(g_dwRamSize-1));
}

extern M64P_RECT frameWriteByCPURect;
extern std::vector<M64P_RECT> frameWriteByCPURects;
extern M64P_RECT frameWriteByCPURectArray[20][20];
extern bool frameWriteByCPURectFlag[20][20];
#define FRAMEBUFFER_IN_BLOCK
bool FrameBufferManager::ProcessFrameWriteRecord()
{
    int size = frameWriteRecord.size();
    if (size == 0)
        return false;

    int index = FindRecentCIInfoIndex(frameWriteRecord[0]);
    if (index == -1)
    {
        LOG_TEXTURE(TRACE1("Frame Buffer Write to non-recorded address = %08X", frameWriteRecord[0]));
        frameWriteRecord.clear();
        return false;
    }
    else
    {
        uint32_t base = g_uRecentCIInfoPtrs[index]->dwAddr;
        uint32_t uwidth = g_uRecentCIInfoPtrs[index]->dwWidth;
        uint32_t uheight = g_uRecentCIInfoPtrs[index]->dwHeight;
        uint32_t upitch = uwidth<<1;

        frameWriteByCPURect.left = uwidth-1;
        frameWriteByCPURect.top  = uheight-1;

        frameWriteByCPURect.right  = 0;
        frameWriteByCPURect.bottom = 0;

        for (int i=0; i<size; i++)
        {
            int off = frameWriteRecord[i]-base;
            if (off < (int)g_uRecentCIInfoPtrs[index]->dwMemSize)
            {
                int y = off/upitch;
                int x = (off - y*upitch)>>1;

#ifdef FRAMEBUFFER_IN_BLOCK
                int xidx=x/32;
                int yidx=y/24;

                M64P_RECT &rect = frameWriteByCPURectArray[xidx][yidx];

                if (!frameWriteByCPURectFlag[xidx][yidx])
                {
                    rect.left=rect.right=x;
                    rect.top=rect.bottom=y;
                    frameWriteByCPURectFlag[xidx][yidx]=true;
                }
                else
                {
                    if(x < rect.left) rect.left = x;
                    if(x > rect.right) rect.right = x;
                    if(y < rect.top) rect.top = y;
                    if(y > rect.bottom) rect.bottom = y;
                }
#else
                if (x < frameWriteByCPURect.left)   frameWriteByCPURect.left = x;
                if (x > frameWriteByCPURect.right)  frameWriteByCPURect.right = x;
                if (y < frameWriteByCPURect.top)    frameWriteByCPURect.top = y;
                if (y > frameWriteByCPURect.bottom) frameWriteByCPURect.bottom = y;
#endif
            }
        }

        frameWriteRecord.clear();
        LOG_TEXTURE(TRACE4("Frame Buffer Write: Left=%d, Top=%d, Right=%d, Bottom=%d", frameWriteByCPURect.left,
            frameWriteByCPURect.top, frameWriteByCPURect.right, frameWriteByCPURect.bottom));
        return true;
    }
}

void FrameBufferManager::FrameBufferReadByCPU( uint32_t addr )
{
    ///return;  // it does not work very well anyway

    if (!frameBufferOptions.bProcessCPURead)
        return;

    addr &= (g_dwRamSize-1);
    int index = FindRecentCIInfoIndex(addr);
    if (index == -1) 
    {
        // Check if this is the depth buffer
        uint32_t size = 2*g_RecentCIInfo[0].dwWidth*g_RecentCIInfo[0].dwHeight;
        addr &= 0x3FFFFFFF;

        if (addr >= g_ZI.dwAddr && addr < g_ZI.dwAddr + size)
        {
            TXTRBUF_OR_CI_DUMP(TRACE1("Depth Buffer read, reported by emulator, address=%08X", addr));
        }
        else
        {
            return;
        }
    }

    if (status.gDlistCount - g_uRecentCIInfoPtrs[index]->lastUsedFrame > 3)
    {
        // Ok, we don't have this frame any more.
        return;
    }

    //TXTRBUF_OR_CI_DUMP(TRACE1("FB Read By CPU at %08X", addr));
    if (g_uRecentCIInfoPtrs[index]->bCopied)
        return;
        
    //if (addr != g_uRecentCIInfoPtrs[index]->dwAddr)
    //    return;

    TXTRBUF_OR_CI_DUMP(TRACE1("Frame Buffer read, reported by emulator, address=%08X", addr));
    uint32_t size = 0x1000 - addr%0x1000;
    CheckAddrInBackBuffers(addr, size, true);
}



extern M64P_RECT frameWriteByCPURect;
extern std::vector<M64P_RECT> frameWriteByCPURects;
extern M64P_RECT frameWriteByCPURectArray[20][20];
extern bool frameWriteByCPURectFlag[20][20];
#define FRAMEBUFFER_IN_BLOCK

void FrameBufferManager::UpdateFrameBufferBeforeUpdateFrame()
{
    if ((frameBufferOptions.bProcessCPUWrite && status.frameWriteByCPU ) ||
        (frameBufferOptions.bLoadBackBufFromRDRAM && !FrameBufferInRDRAMCheckCRC() ))
        // Checks if frame buffer has been modified by CPU
        // Only happens to Dr. Mario
    {
        if (frameBufferOptions.bProcessCPUWrite)
        {
            if (ProcessFrameWriteRecord())
            {
#ifdef FRAMEBUFFER_IN_BLOCK
                for (int i=0; i<20; i++)
                {
                    for (int j=0; j<20; j++)
                    {
                        if (frameWriteByCPURectFlag[i][j])
                        {
                            CRender::GetRender()->DrawFrameBuffer(false, frameWriteByCPURectArray[i][j].left, frameWriteByCPURectArray[i][j].top,
                                frameWriteByCPURectArray[i][j].right-frameWriteByCPURectArray[i][j].left+1, frameWriteByCPURectArray[i][j].bottom-frameWriteByCPURectArray[i][j].top+1);
                        }
                    }
                }
                for (int i=0; i<20; i++)
                {
                    for (int j=0; j<20; j++)
                    {
                        if (frameWriteByCPURectFlag[i][j])
                        {
                            ClearN64FrameBufferToBlack(frameWriteByCPURectArray[i][j].left, frameWriteByCPURectArray[i][j].top,
                                frameWriteByCPURectArray[i][j].right-frameWriteByCPURectArray[i][j].left+1, frameWriteByCPURectArray[i][j].bottom-frameWriteByCPURectArray[i][j].top+1);
                            frameWriteByCPURectFlag[i][j] = false;
                        }
                    }
                }
                //memset(frameWriteByCPURectArray, 0, sizeof(frameWriteByCPURectArray));
                //memset(frameWriteByCPURectFlag, 0, sizeof(frameWriteByCPURectFlag));
#else
                CRender::GetRender()->DrawFrameBuffer(false, frameWriteByCPURect.left, frameWriteByCPURect.top,
                    frameWriteByCPURect.right-frameWriteByCPURect.left, frameWriteByCPURect.bottom-frameWriteByCPURect.top);
                ClearN64FrameBufferToBlack(frameWriteByCPURect.left, frameWriteByCPURect.top,
                    frameWriteByCPURect.right-frameWriteByCPURect.left+1, frameWriteByCPURect.bottom-frameWriteByCPURect.top+1);

                /*
                int size = frameWriteByCPURects.size();
                for( int i=0; i<size; i++)
                {
                    CRender::GetRender()->DrawFrameBuffer(false, frameWriteByCPURects[i].left, frameWriteByCPURects[i].top,
                    frameWriteByCPURects[i].right-frameWriteByCPURects[i].left, frameWriteByCPURects[i].bottom-frameWriteByCPURects[i].top);
                    ClearN64FrameBufferToBlack(frameWriteByCPURects[i].left, frameWriteByCPURects[i].top,
                    frameWriteByCPURects[i].right-frameWriteByCPURects[i].left+1, frameWriteByCPURects[i].bottom-frameWriteByCPURects[i].top+1);
                }
                frameWriteByCPURects.clear();
                */
#endif
            }
            status.frameWriteByCPU = false;
        }
        else
        {
            if (CRender::IsAvailable())
            {
                RecentCIInfo &p = *(g_uRecentCIInfoPtrs[0]);
                CRender::GetRender()->DrawFrameBuffer(false, 0, 0, p.dwWidth, p.dwHeight);
                ClearN64FrameBufferToBlack(0, 0, 0, 0);
            }
        }
    }
}

uint32_t FrameBufferManager::ComputeCImgHeight(SetImgInfo &info, uint32_t &height)
{
    uint32_t dwPC = __RSP.PC[__RSP.PCi];       // This points to the next instruction

    for (int i=0; i<10; i++)
    {
       uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
        uint32_t w0 = *(uint32_t *)(rdram_u8 + dwPC + i*8);
        uint32_t w1 = *(uint32_t *)(rdram_u8 + dwPC + 4 + i*8);

        if ((w0>>24) == G_SETSCISSOR)
        {
            height   = ((w1>>0 )&0xFFF)/4;
            TXTRBUF_DETAIL_DUMP(TRACE1("buffer height = %d", height));
            return G_SETSCISSOR;
        }

        if ((w0>>24) == G_FILLRECT)
        {
            uint32_t x0   = ((w1>>12)&0xFFF)/4;
            uint32_t y0   = ((w1>>0 )&0xFFF)/4;
            uint32_t x1   = ((w0>>12)&0xFFF)/4;
            uint32_t y1   = ((w0>>0 )&0xFFF)/4;

            if (x0 == 0 && y0 == 0)
            {
                if (x1 == info.dwWidth)
                {
                    height = y1;
                    TXTRBUF_DETAIL_DUMP(TRACE1("buffer height = %d", height));
                    return G_FILLRECT;
                }

                if (x1 == (unsigned int)(info.dwWidth-1))
                {
                    height = y1+1;
                    TXTRBUF_DETAIL_DUMP(TRACE1("buffer height = %d", height));
                    return G_FILLRECT;
                }
            }
        }   

        if ((w0>>24) == G_SETCIMG)
        {
            goto step2;
        }

        if ((w0>>24) == G_SETCIMG)
        {
            goto step2;
        }
    }

    if (gRDP.scissor.left == 0 && gRDP.scissor.top == 0 && (unsigned int)gRDP.scissor.right == info.dwWidth)
    {
        height = gRDP.scissor.bottom;
        TXTRBUF_DETAIL_DUMP(TRACE1("buffer height = %d", height));
        return G_SETSCISSOR+1;
    }

step2:
    TXTRBUF_DETAIL_DUMP(TRACE0("Not sure about buffer height"));

    height = info.dwWidth*3/4;
    if (status.dwTvSystem == TV_SYSTEM_PAL)
    {
        height = info.dwWidth*9/11;
    }

    if (gRDP.scissor.bottom < (int)height && gRDP.scissor.bottom != 0)
    {
        height = gRDP.scissor.bottom;
    }

    if (info.dwAddr + height*info.dwWidth*info.dwSize >= g_dwRamSize)
    {
        height = info.dwWidth*3/4;
        if (status.dwTvSystem == TV_SYSTEM_PAL)
        {
            height = info.dwWidth*9/11;
        }

        if (gRDP.scissor.bottom < (int)height && gRDP.scissor.bottom != 0)
        {
            height = gRDP.scissor.bottom;
        }

        if (info.dwAddr + height*info.dwWidth*info.dwSize >= g_dwRamSize)
        {
            height = ( g_dwRamSize - info.dwAddr ) / info.dwWidth;
        }
    }

    TXTRBUF_DETAIL_DUMP(TRACE1("render_texture height = %d", height));
    return 0;
}

int FrameBufferManager::CheckRenderTexturesWithNewCI(SetImgInfo &CIinfo, uint32_t height, bool byNewTxtrBuf)
{
    int matchidx = -1;
    uint32_t memsize = ((height*CIinfo.dwWidth)>>1)<<CIinfo.dwSize;

    for (int i=0; i<numOfTxtBufInfos; i++)
    {
        RenderTextureInfo &info = gRenderTextureInfos[i];
        if (!info.isUsed)
            continue;

        bool covered = false;

        if (info.CI_Info.dwAddr == CIinfo.dwAddr)
        {
            if (info.CI_Info.dwSize == CIinfo.dwSize &&
                info.CI_Info.dwWidth == CIinfo.dwWidth &&
                info.CI_Info.dwFormat == CIinfo.dwFormat &&
                info.N64Height == height 
                )
            {
                // This is the same texture at the same address
                if (byNewTxtrBuf)
                {
                    matchidx = i;
                    break;
                }
            }

            // At the same address, but not the same size
            covered = true;
        }

        if (!covered)
        {
            uint32_t memsize2 = ((info.N64Height*info.N64Width)>>1)<<info.CI_Info.dwSize;

            if (info.CI_Info.dwAddr > CIinfo.dwAddr && info.CI_Info.dwAddr < CIinfo.dwAddr + memsize)
                covered = true;
            else if (info.CI_Info.dwAddr+memsize2 > CIinfo.dwAddr && info.CI_Info.dwAddr+memsize2 < CIinfo.dwAddr + memsize)
                covered = true;
            else if (CIinfo.dwAddr > info.CI_Info.dwAddr && CIinfo.dwAddr < info.CI_Info.dwAddr + memsize2)
                covered = true;
            else if (CIinfo.dwAddr+ memsize > info.CI_Info.dwAddr && CIinfo.dwAddr+ memsize < info.CI_Info.dwAddr + memsize2)
                covered = true;
        }

        if (covered)
        {
            if (info.pRenderTexture->IsBeingRendered())
            {
                TRACE0("Error, covering a render_texture which is being rendered");
                TRACE3("New address=%08X, width=%d, height=%d", CIinfo.dwAddr, CIinfo.dwWidth, height );
                TRACE3("Old address=%08X, width=%d, height=%d", info.CI_Info.dwAddr, info.N64Width, info.N64Height );
            }
            info.isUsed = false;
            TXTRBUF_DUMP(TRACE5("Delete texture buffer %d at %08X, covered by new CI at %08X, Width=%d, Height=%d", 
                i, info.CI_Info.dwAddr, CIinfo.dwAddr, CIinfo.dwWidth, height ));

            if (info.pRenderTexture)
               free(info.pRenderTexture);
            info.pRenderTexture    = NULL;
            info.txtEntry.pTexture = NULL;
            continue;
        }
    }

    return matchidx;
}

extern RecentCIInfo *g_uRecentCIInfoPtrs[5];
RenderTextureInfo newRenderTextureInfo;

int FrameBufferManager::FindASlot(void)
{
    int idx;

    // Find an empty slot
    bool found = false;
    for (int i=0; i<numOfTxtBufInfos; i++)
    {
        if (!gRenderTextureInfos[i].isUsed && gRenderTextureInfos[i].updateAtFrame < status.gDlistCount)
        {
            found = true;
            idx = i;
            break;
        }
    }

    // If cannot find an empty slot, find the oldest slot and reuse the slot
    if (!found)
    {
        uint32_t oldestCount=0xFFFFFFFF;
        uint32_t oldestIdx = 0;
        for (int i=0; i<numOfTxtBufInfos; i++)
        {
            if (gRenderTextureInfos[i].updateAtUcodeCount < oldestCount)
            {
                oldestCount = gRenderTextureInfos[i].updateAtUcodeCount;
                oldestIdx = i;
            }
        }

        idx = oldestIdx;
    }

    if (gRenderTextureInfos[idx].pRenderTexture)
       free(gRenderTextureInfos[idx].pRenderTexture);
    gRenderTextureInfos[idx].pRenderTexture = NULL;

    return idx;
}


void FrameBufferManager::SetRenderTexture(void)
{
    memcpy(&(newRenderTextureInfo.CI_Info), &g_CI, sizeof(SetImgInfo));

    newRenderTextureInfo.N64Width = newRenderTextureInfo.CI_Info.dwWidth;
    newRenderTextureInfo.knownHeight = ComputeCImgHeight(g_CI, newRenderTextureInfo.N64Height);

    status.bHandleN64RenderTexture = true;
    newRenderTextureInfo.maxUsedHeight = 0;

    if (defaultRomOptions.bInN64Resolution)
    {
        newRenderTextureInfo.bufferWidth = newRenderTextureInfo.N64Width;
        newRenderTextureInfo.bufferHeight = newRenderTextureInfo.N64Height;
    }
    else if (defaultRomOptions.bDoubleSizeForSmallTxtrBuf && newRenderTextureInfo.N64Width<=128 && newRenderTextureInfo.N64Height<=128)
    {
        newRenderTextureInfo.bufferWidth = newRenderTextureInfo.N64Width*2;
        newRenderTextureInfo.bufferHeight = newRenderTextureInfo.N64Height*2;
    }
    else
    {
        newRenderTextureInfo.bufferWidth = newRenderTextureInfo.N64Width;
        newRenderTextureInfo.bufferHeight = newRenderTextureInfo.N64Height;
    }

    newRenderTextureInfo.scaleX = newRenderTextureInfo.bufferWidth / float(newRenderTextureInfo.N64Width);
    newRenderTextureInfo.scaleY = newRenderTextureInfo.bufferHeight / float(newRenderTextureInfo.N64Height);

    status.bFrameBufferIsDrawn = false;
    status.bFrameBufferDrawnByTriangles = false;

    newRenderTextureInfo.updateAtFrame = status.gDlistCount;
    newRenderTextureInfo.updateAtUcodeCount = status.gUcodeCount;

    // Delay activation of the render_texture until the 1st rendering
}

int FrameBufferManager::SetBackBufferAsRenderTexture(SetImgInfo &CIinfo, int ciInfoIdx)
{
    // MUDLORD:
    // OK, here's the drill!
    //
    // We  set the graphics card's back buffer's contents as a render_texure
    // This is done due to how the current framebuffer implementation detects
    // changes to the backbuffer memory pointer and then we do a texture
    // copy. This might be slow since it doesn't use hardware auxiliary buffers

    RenderTextureInfo tempRenderTextureInfo;

    memcpy(&(tempRenderTextureInfo.CI_Info), &CIinfo, sizeof(SetImgInfo));

    tempRenderTextureInfo.N64Width = g_uRecentCIInfoPtrs[ciInfoIdx]->dwLastWidth;
    tempRenderTextureInfo.N64Height = g_uRecentCIInfoPtrs[ciInfoIdx]->dwLastHeight;
    tempRenderTextureInfo.knownHeight = true;
    tempRenderTextureInfo.maxUsedHeight = 0;

    tempRenderTextureInfo.bufferWidth = windowSetting.uDisplayWidth;
    tempRenderTextureInfo.bufferHeight = windowSetting.uDisplayHeight;

    tempRenderTextureInfo.scaleX = tempRenderTextureInfo.bufferWidth / float(tempRenderTextureInfo.N64Width);
    tempRenderTextureInfo.scaleY = tempRenderTextureInfo.bufferHeight / float(tempRenderTextureInfo.N64Height);

    status.bFrameBufferIsDrawn = false;
    status.bFrameBufferDrawnByTriangles = false;

    tempRenderTextureInfo.updateAtFrame = status.gDlistCount;
    tempRenderTextureInfo.updateAtUcodeCount = status.gUcodeCount;

    // Checking against previous render_texture infos
    //uint32_t memsize = ((tempRenderTextureInfo.N64Height*tempRenderTextureInfo.N64Width)>>1)<<tempRenderTextureInfo.CI_Info.dwSize;
    int matchidx = CheckRenderTexturesWithNewCI(CIinfo,tempRenderTextureInfo.N64Height,false);
    int idxToUse = (matchidx >= 0) ? matchidx : FindASlot();

    if (gRenderTextureInfos[idxToUse].pRenderTexture == NULL || matchidx < 0)
    {
        gRenderTextureInfos[idxToUse].pRenderTexture = 
        new COGLRenderTexture(tempRenderTextureInfo.bufferWidth, tempRenderTextureInfo.bufferHeight, &gRenderTextureInfos[idxToUse], AS_BACK_BUFFER_SAVE);
    }

    // Need to set all variables for gRenderTextureInfos[idxToUse]
    CRenderTexture *pRenderTexture = gRenderTextureInfos[idxToUse].pRenderTexture;
    memcpy(&gRenderTextureInfos[idxToUse], &tempRenderTextureInfo, sizeof(RenderTextureInfo) );
    gRenderTextureInfos[idxToUse].pRenderTexture = pRenderTexture;
    gRenderTextureInfos[idxToUse].isUsed = true;
    gRenderTextureInfos[idxToUse].txtEntry.pTexture = pRenderTexture->m_pTexture;
    gRenderTextureInfos[idxToUse].txtEntry.txtrBufIdx = idxToUse+1;

    return idxToUse;
}

void FrameBufferManager::CloseRenderTexture(bool toSave)
{
    if (m_curRenderTextureIndex < 0)
        return;

    status.bHandleN64RenderTexture = false;
    if (status.bDirectWriteIntoRDRAM)
    {
        // TODO: Implement
    }
    else 
    {
        RestoreNormalBackBuffer();
        if (!toSave || !status.bFrameBufferIsDrawn || !status.bFrameBufferDrawnByTriangles)
        {
            TXTRBUF_DUMP(TRACE0("Closing render_texture without save"););
            if (gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture)
               free(gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture);
            gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture = NULL;
            gRenderTextureInfos[m_curRenderTextureIndex].isUsed         = false;
            TXTRBUF_DUMP(TRACE1("Delete render_texture %d",m_curRenderTextureIndex););
        }
        else
        {
            TXTRBUF_DUMP(TRACE1("Closing render_texture %d", m_curRenderTextureIndex););
            StoreRenderTextureToRDRAM(-1);

            if (frameBufferOptions.bRenderTextureWriteBack)
            {
               if (gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture)
                  free(gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture);
               gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture = NULL;
               gRenderTextureInfos[m_curRenderTextureIndex].isUsed = false;
               TXTRBUF_DUMP(TRACE1("Delete render_texture %d after writing back to RDRAM",m_curRenderTextureIndex););
            }
            else
            {
                g_pRenderTextureInfo->crcInRDRAM = ComputeRenderTextureCRCInRDRAM(m_curRenderTextureIndex);
                g_pRenderTextureInfo->crcCheckedAtFrame = status.gDlistCount;
            }
        }
    }

    SetScreenMult(windowSetting.uDisplayWidth/windowSetting.fViWidth, windowSetting.uDisplayHeight/windowSetting.fViHeight);
    CRender::g_pRender->UpdateClipRectangle();
    CRender::g_pRender->ApplyScissorWithClipRatio(false);
}

void FrameBufferManager::ClearN64FrameBufferToBlack(uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
   uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
    RecentCIInfo &p = *(g_uRecentCIInfoPtrs[0]);
    uint16_t *frameBufferBase = (uint16_t*)(rdram_u8 + p.dwAddr);
    uint32_t pitch = p.dwWidth;

    if (width == 0 || height == 0)
    {
        uint32_t len = p.dwHeight*p.dwWidth*p.dwSize;

        if (p.dwSize == G_IM_SIZ_4b)
            len = (p.dwHeight*p.dwWidth)>>1;

        memset(frameBufferBase, 0, len);
    }
    else
    {
        for (uint32_t y=0; y<height; y++)
        {
            for (uint32_t x=0; x<width; x++)
            {
                *(frameBufferBase+(y+top)*pitch+x+left) = 0;
            }
        }
    }
}

uint8_t RevTlutTable[0x10000];
bool RevTlutTableNeedUpdate = false;
void InitTlutReverseLookup(void)
{
    if (RevTlutTableNeedUpdate)
    {
        memset(RevTlutTable, 0, 0x10000);
        for (int i=0; i<=0xFF; i++)
        {
            RevTlutTable[g_wRDPTlut[i]] = uint8_t(i);
        }

        RevTlutTableNeedUpdate = false;
    }
}


// Copies backbuffer to N64 framebuffer by notification by emu core
// **buggy**
void FrameBufferManager::CopyBackToFrameBufferIfReadByCPU(uint32_t addr)
{
    int i = FindRecentCIInfoIndex(addr);
    if (i != -1)
    {
        //if (i == 0) CGraphicsContext::Get()->UpdateFrame(false);
        RecentCIInfo *info = g_uRecentCIInfoPtrs[i];

        StoreBackBufferToRDRAM(info->dwAddr, info->dwFormat, info->dwSize, info->dwWidth, info->dwHeight, 
            windowSetting.uDisplayWidth, windowSetting.uDisplayHeight, addr, 0x1000-addr%0x1000, 0, SURFFMT_A8R8G8B8);

        TRACE1("Copy back for CI Address = %08X", info->dwAddr);
    }
}

// We do these checks to see if a render_texture operation is occurring...
void FrameBufferManager::CheckRenderTextureCRCInRDRAM(void)
{
    for (int i=0; i<numOfTxtBufInfos; i++)
    {
        if (!gRenderTextureInfos[i].isUsed)
            continue;

        if (gRenderTextureInfos[i].pRenderTexture->IsBeingRendered())
            continue;

        if (gRenderTextureInfos[i].crcCheckedAtFrame < status.gDlistCount)
        {
            uint32_t crc = ComputeRenderTextureCRCInRDRAM(i);
            if (gRenderTextureInfos[i].crcInRDRAM != crc)
            {
                // RDRAM has been modified by CPU core
                TXTRBUF_DUMP(TRACE2("Delete texture buffer %d at %08X, CRC in RDRAM changed", i, gRenderTextureInfos[i].CI_Info.dwAddr ));

                if (gRenderTextureInfos[i].pRenderTexture)
                   free(gRenderTextureInfos[i].pRenderTexture);
                gRenderTextureInfos[i].pRenderTexture = NULL;
                gRenderTextureInfos[i].isUsed         = false;
                continue;
            }
            else
            {
                gRenderTextureInfos[i].crcCheckedAtFrame = status.gDlistCount;
            }
        }
    }
}

// Check render_texture memory addresses
int FrameBufferManager::CheckAddrInRenderTextures(uint32_t addr, bool checkcrc)
{
    for (int i=0; i<numOfTxtBufInfos; i++)
    {
        if (!gRenderTextureInfos[i].isUsed)
            continue;

        if (gRenderTextureInfos[i].pRenderTexture->IsBeingRendered())
            continue;

        uint32_t bufHeight = gRenderTextureInfos[i].knownHeight ? gRenderTextureInfos[i].N64Height : gRenderTextureInfos[i].maxUsedHeight;
        uint32_t bufMemSize = gRenderTextureInfos[i].CI_Info.dwSize*gRenderTextureInfos[i].N64Width*bufHeight;
        if (addr >=gRenderTextureInfos[i].CI_Info.dwAddr && addr < gRenderTextureInfos[i].CI_Info.dwAddr+bufMemSize)
        {
            if (checkcrc)
            {
                // Check the CRC in RDRAM
                if( gRenderTextureInfos[i].crcCheckedAtFrame < status.gDlistCount )
                {
                    uint32_t crc = ComputeRenderTextureCRCInRDRAM(i);
                    if (gRenderTextureInfos[i].crcInRDRAM != crc)
                    {
                        // RDRAM has been modified by CPU core
                        TRACE3("Buffer %d CRC in RDRAM changed from %08X to %08X", i, gRenderTextureInfos[i].crcInRDRAM, crc );
                        TXTRBUF_DUMP(TRACE2("Delete texture buffer %d at %08X, crcInRDRAM failed.", i, gRenderTextureInfos[i].CI_Info.dwAddr ));

                        if (gRenderTextureInfos[i].pRenderTexture)
                           free(gRenderTextureInfos[i].pRenderTexture);
                        gRenderTextureInfos[i].pRenderTexture = NULL;
                        gRenderTextureInfos[i].isUsed         = false;
                        continue;
                    }
                    else
                    {
                        gRenderTextureInfos[i].crcCheckedAtFrame = status.gDlistCount;
                    }
                }
            }

            TXTRBUF_DUMP(TRACE2("Loading texture address = %08X from texture buffer %d", addr, i));
            return i;
        }
    }

    return -1;
}

// Load texture from render_texture buffer
void FrameBufferManager::LoadTextureFromRenderTexture(TxtrCacheEntry* pEntry, int infoIdx)
{
    if (infoIdx < 0 || infoIdx >= numOfTxtBufInfos)
    {
        infoIdx = CheckAddrInRenderTextures(pEntry->ti.Address, true);
    }

    if (infoIdx >= 0 && gRenderTextureInfos[infoIdx].isUsed && gRenderTextureInfos[infoIdx].pRenderTexture)
    {
        TXTRBUF_DUMP(TRACE1("Loading from render_texture %d", infoIdx));
        gRenderTextureInfos[infoIdx].pRenderTexture->LoadTexture(pEntry);
    }
}

void FrameBufferManager::RestoreNormalBackBuffer()
{
    if (m_curRenderTextureIndex >= 0 && m_curRenderTextureIndex < numOfTxtBufInfos)
    {
        if( gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture )
            gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture->SetAsRenderTarget(false);
        m_isRenderingToTexture = false;
        m_lastTextureBufferIndex = m_curRenderTextureIndex;
    }

    if (!status.bFrameBufferIsDrawn || !status.bFrameBufferDrawnByTriangles)
    {
        gRenderTextureInfos[m_curRenderTextureIndex].isUsed = false;
        TXTRBUF_DUMP(TRACE2("Delete texture buffer %d at %08X, it is never rendered", m_curRenderTextureIndex, gRenderTextureInfos[m_curRenderTextureIndex].CI_Info.dwAddr ));

        if (gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture)
           free(gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture);
        gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture = NULL;
    }
}

uint32_t FrameBufferManager::ComputeRenderTextureCRCInRDRAM(int infoIdx)
{
    if (infoIdx >= numOfTxtBufInfos || infoIdx < 0 || !gRenderTextureInfos[infoIdx].isUsed)
        return 0;

    uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
    RenderTextureInfo &info = gRenderTextureInfos[infoIdx];
    uint32_t height = info.knownHeight ? info.N64Height : info.maxUsedHeight;
    uint8_t *pAddr = (uint8_t*)(rdram_u8 + info.CI_Info.dwAddr);
    uint32_t pitch = (info.N64Width << info.CI_Info.dwSize ) >> 1;

    return CalculateRDRAMCRC(pAddr, 0, 0, info.N64Width, height, info.CI_Info.dwSize, pitch);
}

// Activates texture buffer for drawing
void FrameBufferManager::ActiveTextureBuffer(void)
{
    status.bCIBufferIsRendered = true;

    if (status.bHandleN64RenderTexture)
    {
        // Checking against previous render_texture infos
        int matchidx = -1;

        //uint32_t memsize = ((newRenderTextureInfo.N64Height*newRenderTextureInfo.N64Width)>>1)<<newRenderTextureInfo.CI_Info.dwSize;

        matchidx = CheckRenderTexturesWithNewCI(g_CI, newRenderTextureInfo.N64Height, true);

        int idxToUse=-1;
        if (matchidx >= 0)
        {
            // Reuse the matched slot
            idxToUse = matchidx;
        }
        else
        {
            idxToUse = FindASlot();
        }

        if (gRenderTextureInfos[idxToUse].pRenderTexture == NULL || matchidx < 0)
        {
            int w = newRenderTextureInfo.bufferWidth;
            if (newRenderTextureInfo.knownHeight == G_SETSCISSOR && newRenderTextureInfo.CI_Info.dwAddr == g_ZI.dwAddr)
            {
                w = gRDP.scissor.right;
            }

            gRenderTextureInfos[idxToUse].pRenderTexture = 
                new COGLRenderTexture(w, newRenderTextureInfo.bufferHeight, &gRenderTextureInfos[idxToUse], AS_RENDER_TARGET);
        }

        // Need to set all variables for gRenderTextureInfos[idxToUse]
        CRenderTexture *pRenderTexture = gRenderTextureInfos[idxToUse].pRenderTexture;
        memcpy(&gRenderTextureInfos[idxToUse], &newRenderTextureInfo, sizeof(RenderTextureInfo) );
        gRenderTextureInfos[idxToUse].pRenderTexture = pRenderTexture;
        gRenderTextureInfos[idxToUse].isUsed = true;
        gRenderTextureInfos[idxToUse].txtEntry.pTexture = pRenderTexture->m_pTexture;
        gRenderTextureInfos[idxToUse].txtEntry.txtrBufIdx = idxToUse+1;

        g_pRenderTextureInfo = &gRenderTextureInfos[idxToUse];

        // Active the render_texture
        if (m_curRenderTextureIndex >= 0 && gRenderTextureInfos[m_curRenderTextureIndex].isUsed && gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture)
        {
            gRenderTextureInfos[m_curRenderTextureIndex].pRenderTexture->SetAsRenderTarget(false);
            m_isRenderingToTexture = false;
        }

        if (gRenderTextureInfos[idxToUse].pRenderTexture->SetAsRenderTarget(true))
        {
            m_isRenderingToTexture = true;

            //Clear(CLEAR_COLOR_AND_DEPTH_BUFFER, 0x80808080, 1.0f);
            if (frameBufferOptions.bFillRectNextTextureBuffer)
            {
                CGraphicsContext::g_pGraphicsContext->Clear(CLEAR_COLOR_BUFFER, gRDP.fillColor, 1.0f);
            }
            else if( options.enableHackForGames == HACK_FOR_MARIO_TENNIS && g_pRenderTextureInfo->N64Width > 64 && g_pRenderTextureInfo->N64Width < 300 )
            {
                CGraphicsContext::g_pGraphicsContext->Clear(CLEAR_COLOR_BUFFER, 0, 1.0f);
            }
            else if( options.enableHackForGames == HACK_FOR_MARIO_TENNIS && g_pRenderTextureInfo->N64Width < 64 && g_pRenderTextureInfo->N64Width > 32 )
            {
                CGraphicsContext::g_pGraphicsContext->Clear(CLEAR_COLOR_BUFFER, 0, 1.0f);
            }

            m_curRenderTextureIndex = idxToUse;

            status.bDirectWriteIntoRDRAM = false;

            //SetScreenMult(1, 1);
            SetScreenMult(gRenderTextureInfos[m_curRenderTextureIndex].scaleX, gRenderTextureInfos[m_curRenderTextureIndex].scaleY);
            CRender::g_pRender->UpdateClipRectangle();

            // If needed, draw RDRAM into the render_texture
            //if (frameBufferOptions.bLoadRDRAMIntoRenderTexture)
            //{
            //    CRender::GetRender()->LoadTxtrBufFromRDRAM();
            //}
        }
        else
        {
            if (CDeviceBuilder::m_deviceGeneralType == DIRECTX_DEVICE)
            {
                TRACE1("Error to set Render Target: %d", idxToUse);
                TRACE1("Address = %08X", gRenderTextureInfos[idxToUse].CI_Info.dwAddr);
                TRACE2("Width = %d, Height=%d", gRenderTextureInfos[idxToUse].N64Width, gRenderTextureInfos[idxToUse].N64Height);
            }
        }   
    }
    else
    {
        UpdateRecentCIAddr(g_CI);
        CheckRenderTexturesWithNewCI(g_CI, gRDP.scissor.bottom, false);
    }
}

#define SAVE_CI {g_CI.dwAddr = newCI.dwAddr;g_CI.dwFormat = newCI.dwFormat;g_CI.dwSize = newCI.dwSize;g_CI.dwWidth = newCI.dwWidth;g_CI.bpl=newCI.bpl;}

// Sets CI address for framebuffer copies
void FrameBufferManager::Set_CI_addr(SetImgInfo &newCI)
{
    bool wasDrawingTextureBuffer = status.bN64IsDrawingTextureBuffer;
    status.bN64IsDrawingTextureBuffer = ( newCI.dwSize != G_IM_SIZ_16b || newCI.dwFormat != G_IM_FMT_RGBA || newCI.dwWidth < 200 || ( newCI.dwAddr != g_ZI.dwAddr && newCI.dwWidth != 512 && !g_pFrameBufferManager->HasAddrBeenDisplayed(newCI.dwAddr, newCI.dwWidth)) );
    status.bN64FrameBufferIsUsed = status.bN64IsDrawingTextureBuffer;

    if (!wasDrawingTextureBuffer && g_CI.dwAddr == g_ZI.dwAddr && status.bCIBufferIsRendered)
    {
        TXTRBUF_DUMP(TRACE0("ZI is rendered"));

        if (options.enableHackForGames != HACK_FOR_CONKER && g_uRecentCIInfoPtrs[0]->bCopied == false)
        {
            // Conker is not actually using a backbuffer
            g_pFrameBufferManager->UpdateRecentCIAddr(g_CI);
            if (status.leftRendered != -1 && status.topRendered != -1 && status.rightRendered != -1 && status.bottomRendered != -1)
            {
                M64P_RECT rect={status.leftRendered,status.topRendered,status.rightRendered,status.bottomRendered};
                g_pFrameBufferManager->SaveBackBuffer(0, &rect, false);
            }
            else
            {
                g_pFrameBufferManager->SaveBackBuffer(0, NULL, false);
            }
        }
    }

    frameBufferOptions.bFillRectNextTextureBuffer = false;
    if (g_CI.dwAddr == newCI.dwAddr && status.bHandleN64RenderTexture && (g_CI.dwFormat != newCI.dwFormat || g_CI.dwSize != newCI.dwSize || g_CI.dwWidth != newCI.dwWidth))
    {
        // Mario Tennis player shadow
        g_pFrameBufferManager->CloseRenderTexture(true);
        if (options.enableHackForGames == HACK_FOR_MARIO_TENNIS)
            frameBufferOptions.bFillRectNextTextureBuffer = true;   // Hack for Mario Tennis
    }

    SAVE_CI;

    if (g_CI.dwAddr == g_ZI.dwAddr && !status.bN64IsDrawingTextureBuffer)
    {
        if (g_pFrameBufferManager->IsDIaRenderTexture())
        {
            status.bN64IsDrawingTextureBuffer = true;
            status.bN64FrameBufferIsUsed = status.bN64IsDrawingTextureBuffer;
        }
    }

    status.bCIBufferIsRendered = false;
    status.leftRendered = status.topRendered = status.rightRendered = status.bottomRendered = -1;

    if (currentRomOptions.screenUpdateSetting==SCREEN_UPDATE_AT_CI_CHANGE && !status.bN64IsDrawingTextureBuffer)
    {
        if (status.curRenderBuffer == 0)
        {
            status.curRenderBuffer = g_CI.dwAddr;
        }
        else if (status.curRenderBuffer != g_CI.dwAddr)
        {
            status.curDisplayBuffer = status.curRenderBuffer;
            CGraphicsContext::Get()->UpdateFrame(false);
            status.curRenderBuffer = g_CI.dwAddr;
        }
    }

    if (frameBufferOptions.bAtEachFrameUpdate && !status.bHandleN64RenderTexture)
    {
        if (status.curRenderBuffer != g_CI.dwAddr)
        {
            if (status.gDlistCount%(currentRomOptions.N64FrameBufferWriteBackControl+1) == 0)
            {
                g_pFrameBufferManager->StoreBackBufferToRDRAM(status.curRenderBuffer, 
                    newCI.dwFormat, newCI.dwSize, windowSetting.uViWidth, windowSetting.uViHeight,
                    windowSetting.uDisplayWidth, windowSetting.uDisplayHeight, 0xFFFFFFFF, 0xFFFFFFFF, 0, SURFFMT_A8R8G8B8);
            }
        }

        //status.curDisplayBuffer = status.curRenderBuffer;
        status.curRenderBuffer = g_CI.dwAddr;
    }


    switch (currentRomOptions.N64RenderToTextureEmuType)
    {
    case TXT_BUF_NONE:
        if (status.bHandleN64RenderTexture)
            g_pFrameBufferManager->CloseRenderTexture(false);
        status.bHandleN64RenderTexture = false; // Don't handle N64 render_texture stuffs
        if (!status.bN64IsDrawingTextureBuffer)
            g_pFrameBufferManager->UpdateRecentCIAddr(g_CI);
        break;
    default:
        if (status.bHandleN64RenderTexture)
        {
#ifdef DEBUGGER
            if (pauseAtNext && eventToPause == NEXT_RENDER_TEXTURE)
            {
                pauseAtNext = true;
                eventToPause = NEXT_RENDER_TEXTURE;
            }
#endif
            g_pFrameBufferManager->CloseRenderTexture(true);
        }

        status.bHandleN64RenderTexture = status.bN64IsDrawingTextureBuffer;
        if (status.bHandleN64RenderTexture)
        {
            if (options.enableHackForGames != HACK_FOR_BANJO_TOOIE)
            {
                g_pFrameBufferManager->SetRenderTexture();
            }
        }
        else
        {
#ifdef DEBUGGER
            if (g_CI.dwWidth == 512 && pauseAtNext && (eventToPause==NEXT_OBJ_BG || eventToPause==NEXT_SET_CIMG))
            {
                DebuggerAppendMsg("Warning SetCImg: new Address=0x%08X, Format:%s size=%sb, Width=%d\n", 
                    g_CI.dwAddr, pszImgFormat[newCI.dwFormat], pszImgSize[newCI.dwSize], newCI.dwWidth);
            }
#endif
            //g_pFrameBufferManager->UpdateRecentCIAddr(g_CI);      // Delay this until the CI buffer is actally drawn
        }
        break;
    }
}


void FrameBufferManager::StoreRenderTextureToRDRAM(int infoIdx)
{
    if (!frameBufferOptions.bRenderTextureWriteBack)
        return;

    if (infoIdx < 0)
        infoIdx = m_lastTextureBufferIndex;

    if (!gRenderTextureInfos[infoIdx].pRenderTexture)
        return;

    if (gRenderTextureInfos[infoIdx].pRenderTexture->IsBeingRendered())
    {
        TXTRBUF_DUMP(TRACE1("Cannot SaveTextureBuffer %d, it is being rendered", infoIdx));
        return;
    }

    gRenderTextureInfos[infoIdx].pRenderTexture->StoreToRDRAM(infoIdx);
}


//does FB copy to N64 RDAM structure
void FrameBufferManager::CopyBufferToRDRAM(uint32_t addr, uint32_t fmt, uint32_t siz, uint32_t width, uint32_t height, uint32_t bufWidth, uint32_t bufHeight, uint32_t startaddr, uint32_t memsize, uint32_t pitch, TextureFmt bufFmt, void *buffer, uint32_t bufPitch)
{
    uint32_t startline=0;
    
    if (startaddr == 0xFFFFFFFF)
        startaddr = addr;

    startline = (startaddr-addr)/siz/pitch;
    if (startline >= height)
    {
        //TRACE0("Warning: check me");
        startline = height;
    }

    uint32_t endline = height;
    if (memsize != 0xFFFFFFFF)
    {
        endline = (startaddr+memsize-addr)/siz;
        if (endline % pitch == 0)
            endline /= pitch;
        else
            endline = endline/pitch+1;
    }

    if (endline > height)
    {
        endline = height;
    }

    if (memsize != 0xFFFFFFFF)
    {
        TXTRBUF_DUMP(DebuggerAppendMsg("Start at: 0x%X, from line %d to %d", startaddr-addr, startline, endline););
    }

    int indexes[600];
    {
        float ratio = bufWidth/(float)width;
        for (uint32_t j=0; j<width; j++)
        {
            float sx = j*ratio;
            int sx0 = int(sx+0.5);
            indexes[j] = 4*sx0;
        }
    }

    if (siz == G_IM_SIZ_16b)
    {
       uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
        uint16_t *frameBufferBase = (uint16_t*)(rdram_u8 + addr);

        if (bufFmt == TEXTURE_FMT_A8R8G8B8)
        {
            float ratio = bufHeight/(float)height;

            for (uint32_t i=startline; i<endline; i++)
            {
                int sy0 = int(i*ratio+0.5);

                uint16_t *pD = frameBufferBase + i * pitch;
                uint8_t *pS0 = (uint8_t *)buffer + sy0 * bufPitch;

                for (uint32_t j=0; j<width; j++)
                {
                    // Point
                    uint8_t r = pS0[indexes[j]+2];
                    uint8_t g = pS0[indexes[j]+1];
                    uint8_t b = pS0[indexes[j]+0];
                    uint8_t a = pS0[indexes[j]+3];

                    // Liner
                    *(pD+(j^1)) = ConvertRGBATo555( r, g, b, a);
                }
            }
        }
        else
        {
            TRACE1("Copy %sb FrameBuffer to RDRAM, not implemented", pszImgSize[siz]);
        }
    }
    else if (siz == G_IM_SIZ_8b && fmt == G_IM_FMT_CI)
    {
       uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
        uint8_t *frameBufferBase = (uint8_t*)(rdram_u8 + addr);

        if (bufFmt == TEXTURE_FMT_A8R8G8B8)
        {
            uint16_t tempword;
            InitTlutReverseLookup();

            for (uint32_t i=startline; i<endline; i++)
            {
                uint8_t *pD = frameBufferBase + i * width;
                uint8_t *pS = (uint8_t *)buffer + i*bufHeight/height * bufPitch;
                for (uint32_t j=0; j<width; j++)
                {
                    int pos = 4*(j*bufWidth/width);

                    tempword = ConvertRGBATo555((pS[pos+2]),    /* Red */
                                                (pS[pos+1]),    /* Green */
                                                (pS[pos+0]),    /* Blue */
                                                (pS[pos+3]));   /* Alpha */

                    //*pD = CIFindIndex(tempword);
                    *(pD+(j^3)) = RevTlutTable[tempword];
                }
            }
        }
    }
    else if (siz == G_IM_SIZ_8b && fmt == G_IM_FMT_I)
    {
       uint8_t *rdram_u8 = (uint8_t*)gfx_info.RDRAM;
        uint8_t *frameBufferBase = (uint8_t*)(rdram_u8 + addr);

        if (bufFmt == TEXTURE_FMT_A8R8G8B8)
        {
            float ratio = bufHeight/(float)height;

            for (uint32_t i=startline; i<endline; i++)
            {
                int sy0 = int(i*ratio+0.5);

                uint8_t *pD = frameBufferBase + i * width;
                uint8_t *pS0 = (uint8_t *)buffer + sy0 * bufPitch;

                for (uint32_t j=0; j<width; j++)
                {
                    // Point
                    uint32_t r = pS0[indexes[j]+2];
                    uint32_t g = pS0[indexes[j]+1];
                    uint32_t b = pS0[indexes[j]+0];

                    // Liner
                    *(pD+(j^3)) = (uint8_t)((r+b+g)/3);
                }
            }
        }
    }
}


#ifdef DEBUGGER
void FrameBufferManager::DisplayRenderTexture(int infoIdx)
{
    if (infoIdx < 0)
        infoIdx = m_lastTextureBufferIndex;

    if (gRenderTextureInfos[infoIdx].pRenderTexture)
    {
        if (gRenderTextureInfos[infoIdx].pRenderTexture->IsBeingRendered())
        {
            TRACE1("Render texture %d is being rendered, cannot display", infoIdx);
        }
        else
        {
            TRACE1("Texture buffer %d:", infoIdx);
            TRACE1("Address=%08X", gRenderTextureInfos[infoIdx].CI_Info.dwAddr);
            TRACE2("Width=%d, Created Height=%d", gRenderTextureInfos[infoIdx].N64Width, gRenderTextureInfos[infoIdx].N64Height);
            TRACE2("Format=%d, Size=%d", gRenderTextureInfos[infoIdx].CI_Info.dwFormat, gRenderTextureInfos[infoIdx].CI_Info.dwSize);
        }
    }
    else
    {
        TRACE1("Texture buffer %d is not used", infoIdx);
    }
}
#endif



// Saves backbuffer
// this is the core to the current framebuffer code
// We need to save backbuffer when changed by framebuffer
// so that we can use it for framebuffer effects
void FrameBufferManager::SaveBackBuffer(int ciInfoIdx, M64P_RECT* pSrcRect, bool forceToSaveToRDRAM)
{
    RecentCIInfo &ciInfo = *g_uRecentCIInfoPtrs[ciInfoIdx];

    if (ciInfoIdx == 1)    // to save the current front buffer
    {
        CGraphicsContext::g_pGraphicsContext->UpdateFrame(true);
    }

    if (frameBufferOptions.bWriteBackBufToRDRAM || forceToSaveToRDRAM)
    {
        uint32_t width = ciInfo.dwWidth;
        uint32_t height = ciInfo.dwHeight;

        if (ciInfo.dwWidth == *gfx_info.VI_WIDTH_REG && ciInfo.dwWidth != windowSetting.uViWidth)
        {
            width = windowSetting.uViWidth;
            height = windowSetting.uViHeight;
        }

        StoreBackBufferToRDRAM( ciInfo.dwAddr, ciInfo.dwFormat, ciInfo.dwSize, width, height, 
            windowSetting.uDisplayWidth, windowSetting.uDisplayHeight, 0xFFFFFFFF, 0xFFFFFFFF, 0, SURFFMT_A8R8G8B8);

        g_uRecentCIInfoPtrs[ciInfoIdx]->bCopied = true;
        if (ciInfoIdx == 1)    // to save the current front buffer
        {
            CGraphicsContext::g_pGraphicsContext->UpdateFrame(true);
        }
        return;
    }


    SetImgInfo tempinfo;
    tempinfo.dwAddr = ciInfo.dwAddr;
    tempinfo.dwFormat = ciInfo.dwFormat;
    tempinfo.dwSize = ciInfo.dwSize;
    tempinfo.dwWidth = ciInfo.dwWidth;

    int idx = SetBackBufferAsRenderTexture(tempinfo, ciInfoIdx);

    CopyBackBufferToRenderTexture(idx, ciInfo, pSrcRect);

    gRenderTextureInfos[idx].crcCheckedAtFrame = status.gDlistCount;
    gRenderTextureInfos[idx].crcInRDRAM = ComputeRenderTextureCRCInRDRAM(idx);

    g_uRecentCIInfoPtrs[ciInfoIdx]->bCopied = true;
}

