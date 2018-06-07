/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2014 - 2018 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************
*
*    The GPL License (GPL)
*
*    Copyright (C) 2014 - 2018 Vivante Corporation
*
*    This program is free software; you can redistribute it and/or
*    modify it under the terms of the GNU General Public License
*    as published by the Free Software Foundation; either version 2
*    of the License, or (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*****************************************************************************
*
*    Note: This software is released under dual MIT and GPL licenses. A
*    recipient may use this file under the terms of either the MIT license or
*    GPL License. If you wish to use only one license not the other, you can
*    indicate your decision by deleting one of the above license notices in your
*    version of this file.
*
*****************************************************************************/


#include "gc_hal.h"
#include "gc_hal_kernel.h"
#include "gc_hal_kernel_context.h"

#define _GC_OBJ_ZONE    gcvZONE_HARDWARE

typedef struct _gcsMCFE_DESCRIPTOR
{
    gctUINT32       start;
    gctUINT32       end;
}
gcsMCFE_DESCRIPTOR;

/* 2^DepthExp = Depth. */
#define MCFE_RINGBUF_DEPTH_EXP         9
/* Depth. */
#define MCFE_RINGBUF_DEPTH             (1 << MCFE_RINGBUF_DEPTH_EXP)
/* MCFE descriptor size in bytes, fixed 8. */
#define MCFE_COMMAND_DESC_SIZE         8
/* FIFO size in bytes. */
#define MCFE_RINGBUF_SIZE              (MCFE_RINGBUF_DEPTH * MCFE_COMMAND_DESC_SIZE)

typedef struct _gcsMCFE_RING_BUF
{
    gckVIDMEM_NODE  ringBufVideoMem;
    gctUINT32       ringBufAddress;
    gctUINT32 *     ringBufLogical;
    gctSIZE_T       ringBufBytes;

    gctUINT32       gpuAddress;
    gctPHYS_ADDR_T  physical;

    /* Read ptr should be often out-of-date. */
    gctUINT32       readPtr;
    gctUINT32       writePtr;
}
gcsMCFE_RING_BUF;

typedef struct _gcsMCFE_CHANNEL
{
    gceMCFE_CHANNEL_TYPE binding;
    gcsMCFE_RING_BUF stdRingBuf;
    gcsMCFE_RING_BUF priRingBuf;
}
gcsMCFE_CHANNEL;

struct _gckMCFE
{
    gctUINT32 channelCount;
    gctBOOL   mmuEnabled;

    /*
     * Channels must be the last field.
     * Will allocate struct size according to channel count.
     */
    gcsMCFE_CHANNEL channels[1];
};

static gcmINLINE gctUINT32
_NextPtr(
    gctUINT32 Ptr
    )
{
    return (Ptr + 1) & (MCFE_RINGBUF_DEPTH - 1);
}

static gceSTATUS
_AllocateDescRingBuf(
    gckHARDWARE Hardware,
    gcsMCFE_RING_BUF * Channel
    )
{
    gceSTATUS status;
    gcePOOL pool = gcvPOOL_DEFAULT;
    gckKERNEL kernel = Hardware->kernel;

    Channel->ringBufBytes = MCFE_RINGBUF_SIZE;

    /* Allocate video memory node for mcfe ring buffer. */
    gcmkONERROR(gckKERNEL_AllocateVideoMemory(
        kernel,
        64,
        gcvVIDMEM_TYPE_COMMAND,
        0,
        &Channel->ringBufBytes,
        &pool,
        &Channel->ringBufVideoMem
        ));

    /* Lock for GPU access. */
    gcmkONERROR(gckVIDMEM_NODE_Lock(
        kernel,
        Channel->ringBufVideoMem,
        &Channel->gpuAddress
        ));

    /* Lock for kernel side CPU access. */
    gcmkONERROR(gckVIDMEM_NODE_LockCPU(
        kernel,
        Channel->ringBufVideoMem,
        gcvFALSE,
        gcvFALSE,
        (gctPOINTER *)&Channel->ringBufLogical
        ));

    /* Get CPU physical address. */
    gcmkONERROR(gckVIDMEM_NODE_GetPhysical(
        kernel,
        Channel->ringBufVideoMem,
        0,
        &Channel->physical
        ));

    gcmkVERIFY_OK(gckOS_CPUPhysicalToGPUPhysical(
        Hardware->os, Channel->physical, &Channel->physical));

    if (Channel->physical > 0xffffffffull)
    {
        gcmkPRINT("%s(%d): MCFE ring buffer physical over 4G: 0x%llx",
            __FUNCTION__, __LINE__, (unsigned long long)Channel->physical);
    }

    /* Default to use physical. */
    Channel->ringBufAddress = (gctUINT32)Channel->physical;

    return gcvSTATUS_OK;

OnError:
    return status;
}

static void
_DestroyDescRingBuf(
    gckHARDWARE Hardware,
    gcsMCFE_RING_BUF * Channel
    )
{
    gckKERNEL kernel = Hardware->kernel;

    if (Channel->ringBufVideoMem)
    {
        gcmkVERIFY_OK(gckVIDMEM_NODE_UnlockCPU(
            kernel,
            Channel->ringBufVideoMem,
            0,
            gcvFALSE
            ));

        gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(
            kernel,
            Channel->ringBufVideoMem
            ));

        Channel->ringBufVideoMem = gcvNULL;
        Channel->ringBufLogical  = gcvNULL;
    }
}

static gcmINLINE void
_DestroyMCFE(
    IN gckHARDWARE Hardware,
    IN gckMCFE FE
    )
{
    if (FE)
    {
        gctUINT i;

        for (i = 0; i < FE->channelCount; i++)
        {
            if (FE->channels[i].binding)
            {
                _DestroyDescRingBuf(Hardware, &FE->channels[i].stdRingBuf);
                _DestroyDescRingBuf(Hardware, &FE->channels[i].priRingBuf);
            }
        }

        gcmkOS_SAFE_FREE(Hardware->os, FE);
    }
}


static gceSTATUS
_ConstructChannel(
    IN gckHARDWARE Hardware,
    IN gceMCFE_CHANNEL_TYPE ChannelType,
    IN gcsMCFE_CHANNEL * Channel
    )
{
    Channel->binding = ChannelType;

    return gcvSTATUS_OK;
}

gceSTATUS
gckMCFE_Construct(
    IN gckHARDWARE Hardware,
    OUT gckMCFE *FE
    )
{
    gceSTATUS status;
    gckMCFE fe = gcvNULL;
    gctUINT32 i;
    gctSIZE_T size = sizeof(struct _gckMCFE);

    if (Hardware->mcfeChannelCount > 1)
    {
        size += sizeof(gcsMCFE_CHANNEL) * (Hardware->mcfeChannelCount - 1);
    }

    gcmkONERROR(gckOS_Allocate(Hardware->os,
                               size,
                               (gctPOINTER *)&fe));

    gckOS_ZeroMemory(fe, size);

    fe->channelCount = Hardware->mcfeChannelCount;

    for (i = 0; i < fe->channelCount; i++)
    {
        gcmkONERROR(
            _ConstructChannel(Hardware,
                              Hardware->mcfeChannels[i],
                              &fe->channels[i]));
    }

    /* Enable all events. */
    gcmkONERROR(
        gckOS_WriteRegisterEx(Hardware->os,
                              Hardware->core,
                              0x00014,
                              0xFFFFFFFF));

    *FE = fe;
    return gcvSTATUS_OK;

OnError:
    _DestroyMCFE(Hardware, fe);
    return status;
}

void
gckMCFE_Destroy(
    IN gckHARDWARE Hardware,
    IN gckMCFE FE
    )
{
    _DestroyMCFE(Hardware, FE);
}

static gceSTATUS
_ProgramDescRingBuf(
    IN gckHARDWARE Hardware,
    IN gctBOOL MMUEnabled,
    IN gcsMCFE_RING_BUF * Channel,
    IN gctUINT32 Index,
    IN gctBOOL Priority
    )
{
#ifdef GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address
    gctUINT32 ringBufStartReg;
    gctUINT32 depthExpReg;
    gctUINT32 readPtrReg;
    gctUINT32 writePtrReg;
    gctUINT32 data = 0;

    if (Priority)
    {
        ringBufStartReg = GCREG_MCFE_PRI_DESC_RING_BUF_START_ADDR_Address;
        depthExpReg     = GCREG_MCFE_PRI_DESC_FIFO_DEPTH_EXP_Address;
        readPtrReg      = GCREG_MCFE_PRI_DESC_FIFO_RD_PTR_Address;
        writePtrReg     = GCREG_MCFE_PRI_DESC_FIFO_WR_PTR_Address;
    }
    else
    {
        ringBufStartReg = GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address;
        depthExpReg     = GCREG_MCFE_STD_DESC_FIFO_DEPTH_EXP_Address;
        readPtrReg      = GCREG_MCFE_STD_DESC_FIFO_RD_PTR_Address;
        writePtrReg     = GCREG_MCFE_STD_DESC_FIFO_WR_PTR_Address;
    }

    ringBufStartReg += Index << 2;
    depthExpReg     += Index << 2;
    readPtrReg      += Index << 2;
    writePtrReg     += Index << 2;

    Channel->ringBufAddress = MMUEnabled ? Channel->gpuAddress
                            : (gctUINT32)Channel->physical;

    /* Channel ringBuf start address. */
    gcmkVERIFY_OK(gckOS_WriteRegisterEx(
        Hardware->os, Hardware->core, ringBufStartReg, Channel->ringBufAddress));

    /* Channel ringBuf depth (exponent of 2). */
    gcmkVERIFY_OK(gckOS_WriteRegisterEx(
        Hardware->os, Hardware->core, depthExpReg, MCFE_RINGBUF_DEPTH_EXP));

    /* The RD ptr could keep unchanged, read and compute WR ptr. */
    gcmkVERIFY_OK(gckOS_ReadRegisterEx(Hardware->os, Hardware->core, readPtrReg, &data));

    /* Priority ring buffer write ptr. */
    /* gcmkVERIFY_OK(gckOS_WriteRegisterEx(Hardware->os, Hardware->core, writePtrReg, data)); */

    /* No valid descriptor initially. */
    Channel->readPtr = Channel->writePtr = data;

    return gcvSTATUS_OK;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

static gceSTATUS
_InitializeChannel(
    IN gckHARDWARE Hardware,
    IN gctBOOL MMUEnabled,
    IN gcsMCFE_CHANNEL * Channel,
    IN gctUINT32 Index
    )
{
    gceSTATUS status;

    /* Allocate ring buffer descriptor memory. */
    if (!Channel->stdRingBuf.ringBufVideoMem)
    {
        gcmkONERROR(_AllocateDescRingBuf(Hardware, &Channel->stdRingBuf));
    }

    /* No priority channel in system engine. */
    if (!Channel->priRingBuf.ringBufVideoMem && Index != 0)
    {
        gcmkONERROR(_AllocateDescRingBuf(Hardware, &Channel->priRingBuf));
    }

    gcmkONERROR(_ProgramDescRingBuf(Hardware, MMUEnabled, &Channel->stdRingBuf, Index, gcvFALSE));

    /* No priority channel in system engine. */
    if (Channel->binding != gcvMCFE_CHANNEL_SYSTEM)
    {
        gcmkONERROR(_ProgramDescRingBuf(Hardware, MMUEnabled, &Channel->priRingBuf, Index, gcvTRUE));
    }

    return gcvSTATUS_OK;

OnError:
    /* It's OK to leave ringBuf memory not free'd here. */
    return status;
}

gceSTATUS
gckMCFE_Initialize(
    IN gckHARDWARE Hardware,
    IN gctBOOL MMUEnabled,
    IN gckMCFE FE
    )
{
    gctUINT32 i;
    gceSTATUS status;

    for (i = 0; i < FE->channelCount; i++)
    {
        if (FE->channels[i].binding)
        {
            gcmkONERROR(_InitializeChannel(Hardware, MMUEnabled, &FE->channels[i], i));
        }
    }

    FE->mmuEnabled = MMUEnabled;
    return gcvSTATUS_OK;

OnError:
    return status;
}

gceSTATUS
gckMCFE_Nop(
    IN gckHARDWARE Hardware,
    IN gctPOINTER Logical,
    IN OUT gctSIZE_T * Bytes
    )
{
#ifdef GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address
    gctUINT32_PTR logical = (gctUINT32_PTR) Logical;
    gceSTATUS status;

    gcmkHEADER_ARG("Hardware=0x%x Logical=0x%x *Bytes=%lu",
                   Hardware, Logical, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
    gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));

    if (Logical != gcvNULL)
    {
        if (*Bytes < 8)
        {
            /* Command queue too small. */
            gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
        }

        /* Append NOP. */
        logical[0] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_NOP & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)));

        logical[1] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_NOP & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)));


        gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_HARDWARE, "0x%x: NOP", Logical);
    }

    if (Bytes != gcvNULL)
    {
        /* Return number of bytes required by the NOP command. */
        *Bytes = 8;
    }

    /* Success. */
    gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}


gceSTATUS
gckMCFE_Event(
    IN gckHARDWARE Hardware,
    IN gctPOINTER Logical,
    IN gctUINT8 Event,
    IN gceKERNEL_WHERE FromWhere,
    IN OUT gctUINT32 * Bytes
    )
{
#ifdef GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address
    gctUINT size;
    gctUINT32_PTR logical = (gctUINT32_PTR) Logical;
    gceSTATUS status;

    gcmkHEADER_ARG("Hardware=0x%x Logical=0x%x Event=%u FromWhere=%d *Bytes=%lu",
                   Hardware, Logical, Event, FromWhere, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
    gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));
    gcmkVERIFY_ARGUMENT(Event < 32);

    /* Ignored. */
    (void)FromWhere;

    size = 8;

    if (Logical != gcvNULL)
    {
        if (*Bytes < size)
        {
            /* Command queue too small. */
            gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
        }

        /* Append EVENT(Event). */
        logical[0] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_SUB_COMMAND & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)))
                   | ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1) == 32) ?
 ~0U : (~(~0U << ((1 ? MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1))))))) << (0 ?
 MCFE_COMMAND_SUB_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_SUB_OPCODE_INTERRUPT_EVENT & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1) == 32) ?
 ~0U : (~(~0U << ((1 ? MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1))))))) << (0 ?
 MCFE_COMMAND_SUB_OPCODE)))
                   | Event;

        logical[1] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_NOP & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)));


#if gcmIS_DEBUG(gcdDEBUG_TRACE)
        {
            gctPHYS_ADDR_T phys;
            gckOS_GetPhysicalAddress(Hardware->os, Logical, &phys);
            gckOS_CPUPhysicalToGPUPhysical(Hardware->os, phys, &phys);
            gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_HARDWARE,
                           "0x%08x: EVENT %d", phys, Event);
        }
#endif

#if gcdINTERRUPT_STATISTIC
        if (Event < gcmCOUNTOF(Hardware->kernel->eventObj->queues))
        {
            gckOS_AtomSetMask(Hardware->pendingEvent, 1 << Event);
        }
#endif
    }

    if (Bytes != gcvNULL)
    {
        /* Return number of bytes required by the EVENT command. */
        *Bytes = size;
    }

    /* Success. */
    gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

gceSTATUS
gckMCFE_SendSemaphore(
    IN gckHARDWARE Hardware,
    IN gctPOINTER Logical,
    IN gctUINT32 SemaId,
    IN OUT gctUINT32 * Bytes
    )
{
#ifdef GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address
    gctUINT32_PTR logical = (gctUINT32_PTR) Logical;
    gceSTATUS status;

    gcmkHEADER_ARG("Hardware=0x%x Logical=0x%x SemaId=%u *Bytes=%lu",
                   Hardware, Logical, SemaId, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
    gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));
    gcmkVERIFY_ARGUMENT(SemaId < 0xFFFF);

    if (Logical != gcvNULL)
    {
        if (*Bytes < 8)
        {
            /* Command queue too small. */
            gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
        }

        /* Append SEND_SEMAPHORE(SemaId). */
        logical[0] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_SUB_COMMAND & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)))
                   | ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1) == 32) ?
 ~0U : (~(~0U << ((1 ? MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1))))))) << (0 ?
 MCFE_COMMAND_SUB_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_SUB_OPCODE_SEND_SEMAPHORE & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1) == 32) ?
 ~0U : (~(~0U << ((1 ? MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1))))))) << (0 ?
 MCFE_COMMAND_SUB_OPCODE)))
                   | SemaId;

        logical[1] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_NOP & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)));

    }

    if (Bytes != gcvNULL)
    {
        *Bytes = 8;
    }

    /* Success. */
    gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

gceSTATUS
gckMCFE_WaitSemaphore(
    IN gckHARDWARE Hardware,
    IN gctPOINTER Logical,
    IN gctUINT32 SemaId,
    IN OUT gctUINT32 * Bytes
    )
{
#ifdef GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address
    gctUINT32_PTR logical = (gctUINT32_PTR) Logical;
    gceSTATUS status;

    gcmkHEADER_ARG("Hardware=0x%x Logical=0x%x SemaId=%u *Bytes=%lu",
                   Hardware, Logical, SemaId, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
    gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));
    gcmkVERIFY_ARGUMENT(SemaId < 0xFFFF);

    if (Logical != gcvNULL)
    {
        if (*Bytes < 8)
        {
            /* Command queue too small. */
            gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
        }

        /* Append WAIT_SEMAPHORE(SemaId). */
        logical[0] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_SUB_COMMAND & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)))
                   | ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1) == 32) ?
 ~0U : (~(~0U << ((1 ? MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1))))))) << (0 ?
 MCFE_COMMAND_SUB_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_SUB_OPCODE_WAIT_SEMAPHORE & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1) == 32) ?
 ~0U : (~(~0U << ((1 ? MCFE_COMMAND_SUB_OPCODE) - (0 ? MCFE_COMMAND_SUB_OPCODE) + 1))))))) << (0 ?
 MCFE_COMMAND_SUB_OPCODE)))
                   | SemaId;

        logical[1] = ((((gctUINT32) (0)) & ~(((gctUINT32) (((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE))) | (((gctUINT32) (MCFE_COMMAND_OPCODE_NOP & ((gctUINT32) ((((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1) == 32) ? ~0U : (~(~0U << ((1 ?
 MCFE_COMMAND_OPCODE) - (0 ? MCFE_COMMAND_OPCODE) + 1))))))) << (0 ? MCFE_COMMAND_OPCODE)));

    }

    if (Bytes != gcvNULL)
    {
        *Bytes = 8;
    }

    /* Success. */
    gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

gceSTATUS
gckMCFE_Execute(
    IN gckHARDWARE Hardware,
    IN gctBOOL Priority,
    IN gctUINT32 ChannelId,
    IN gctUINT32 Address,
    IN gctUINT32 Bytes
    )
{
#ifdef GCREG_MCFE_STD_DESC_RING_BUF_START_ADDR_Address
    gctUINT32 regBase;
    gcsMCFE_DESCRIPTOR *desc;
    gcsMCFE_CHANNEL * channel  = &Hardware->mcFE->channels[ChannelId];
    gcsMCFE_RING_BUF * ringBuf = Priority ? &channel->priRingBuf
                              : &channel->stdRingBuf;

    /* No priority channel in system channel by design. */
    gcmkASSERT(!(channel->binding == gcvMCFE_CHANNEL_SYSTEM && Priority == 1));

    while (_NextPtr(ringBuf->writePtr) == ringBuf->readPtr)
    {
        gctUINT32 data;
        regBase = Priority ? GCREG_MCFE_PRI_DESC_FIFO_RD_PTR_Address
                : GCREG_MCFE_STD_DESC_FIFO_RD_PTR_Address;

        gcmkVERIFY_OK(gckOS_ReadRegisterEx(Hardware->os,
                                           Hardware->core,
                                           regBase + ChannelId * 4,
                                           &data));

        ringBuf->readPtr = data;

        if (_NextPtr(ringBuf->writePtr) == ringBuf->readPtr)
        {
            gcmkPRINT("%s: MCFE channel %s-%d ringBuf is full!",
                      __FUNCTION__,
                      Priority ? "Pri" : "Std",
                      ChannelId);

            gckOS_Delay(Hardware->os, 100);
        }
    }

    regBase = Priority ? GCREG_MCFE_PRI_DESC_FIFO_WR_PTR_Address
            : GCREG_MCFE_STD_DESC_FIFO_WR_PTR_Address;

    /* ringBufLogical is in uint32, 2 uint32 contributes 1 descriptr. */
    desc = (gcsMCFE_DESCRIPTOR *)&ringBuf->ringBufLogical[ringBuf->writePtr * 2];
    desc->start = Address;
    desc->end   = Address + Bytes;

    gcmkDUMP(Hardware->os,
             "#[descriptor %d: channel %s-%d]",
             ringBuf->writePtr,
             Priority ? "Pri" : "Std",
             ChannelId);

    gcmkDUMP_BUFFER(Hardware->os,
                    gcvDUMP_BUFFER_KERNEL_COMMAND,
                    desc,
                    ringBuf->ringBufAddress + ringBuf->writePtr * 8,
                    8);

    gckOS_MemoryBarrier(Hardware->os, desc);

    ringBuf->writePtr = _NextPtr(ringBuf->writePtr);

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_HARDWARE,
                   "0x%08X - 0x%08X: %06d bytes, Channel=%s-%d",
                   desc->start, desc->end, Bytes,
                   Priority ? "Pri" : "Std", ChannelId);

    gcmkVERIFY_OK(gckOS_WriteRegisterEx(Hardware->os,
                                        Hardware->core,
                                        regBase + ChannelId * 4,
                                        ringBuf->writePtr));

    return gcvSTATUS_OK;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}


