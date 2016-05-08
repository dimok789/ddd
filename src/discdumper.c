#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <malloc.h>
#include "common/kernel_defs.h"
#include "dynamic_libs/os_functions.h"
#include "dynamic_libs/fs_functions.h"
#include "dynamic_libs/aoc_functions.h"
#include "dynamic_libs/socket_functions.h"
#include "dynamic_libs/sys_functions.h"
#include "game/rpx_rpl_table.h"
#include "game/memory_area_table.h"
#include "utils/net.h"
#include "utils/utils.h"
#include "discdumper.h"

#define TITLE_LOCATION_ODD          0
#define TITLE_LOCATION_USB          1
#define TITLE_LOCATION_MLC          2

#define BUFFER_SIZE                 0x1000

extern ReducedCosAppXmlInfo cosAppXmlInfoStruct;

static int iClientSocket = -1;
static u8 u8DumpingPaths = 0;
static u8 u8TitleLocation = 0;
static u64 u64TitleMetaDataPending = 0;

//! setup some default IP
static u32 serverIpAddress = 0xC0A8B203;

static void DumpFile(void *pClient, void *pCmd, SendData *sendData, char *pPath, unsigned int fileSize)
{
    sendData->tag = 0x02;
    memcpy(&sendData->data[0], &fileSize, 4);
    sendData->length = snprintf((char*)sendData->data + 4, BUFFER_SIZE - 4, "%s", pPath) + 4 + 1;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

    int ret = recvwait(iClientSocket, (char*)sendData, sizeof(SendData) + 1);
    if(ret < (int)(sizeof(SendData) + 1) || (sendData->data[0] != 1))
    {
        return;
    }

    unsigned char* dataBuf = (unsigned char*)memalign(0x40, BUFFER_SIZE);
    if(!dataBuf) {
        return;
    }

    int fd = 0;
    if (FSOpenFile(pClient, pCmd, pPath, "r", &fd, -1) != FS_STATUS_OK)
    {
        free(dataBuf);
        sendData->tag = 0x04;
        sendData->length = 0;
        sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);
        return;
    }

    unsigned int size = 0;

    // Copy rpl in memory
    while ((ret = FSReadFile(pClient, pCmd, dataBuf, 0x1, BUFFER_SIZE, fd, 0, -1)) > 0)
    {
        size += ret;

        sendData->tag = 0x03;
        sendData->length = ret;
        memcpy(sendData->data, dataBuf, sendData->length);

        if(sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length) < 0) {
            break;
        }
    }

    sendData->tag = 0x04;
    sendData->length = 0;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

    FSCloseFile(pClient, pCmd, fd, -1);
    free(dataBuf);
}

static int DumpDir(void *pClient, void *pCmd, SendData *sendData, char *pPath)
{
    int iDh = 0;

    sendData->tag = 0x01;
    sendData->length = snprintf((char*)sendData->data, BUFFER_SIZE, "%s", pPath) + 1;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

    if(FSOpenDir(pClient, pCmd, pPath, &iDh, -1) != 0) {
        return -1;
    }

    FSDirEntry * dirEntry = (FSDirEntry*) malloc(sizeof(FSDirEntry));

    while(FSReadDir(pClient, pCmd, iDh, dirEntry, -1) == 0)
    {
        int len = strlen(pPath);
        snprintf(pPath + len, FS_MAX_FULLPATH_SIZE - len, "/%s", dirEntry->name);

        if(dirEntry->stat.flag & 0x80000000) {
            DumpDir(pClient, pCmd, sendData, pPath);
        }
        else {
            DumpFile(pClient, pCmd, sendData, pPath, dirEntry->stat.size);
        }
        pPath[len] = 0;
    }
    free(dirEntry);
    FSCloseDir(pClient, pCmd, iDh, -1);
    return 0;
}

static void DumpCodeXmls(SendData *sendData)
{
    //!----------------------------------------------------------------------------------------------------------------------------------------------------------------
    //! create code folder if it doesn't exist yet
    //!----------------------------------------------------------------------------------------------------------------------------------------------------------------
    sendData->tag = 0x01;
    sendData->length = snprintf((char*)sendData->data, BUFFER_SIZE, "/vol/code") + 1;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

    //! get full argstr
    u32 maxArgStrLen = 0x1000;
    char *argstr = (char*)malloc(maxArgStrLen);
    if(argstr)
    {
        int argc = 0;
        char** argv = 0;

        OSGetArgcArgv(&argc, &argv);

        char *writePos = argstr;
        for (int i = 0; i < argc; i++)
        {
            if (i > 0)
                writePos += snprintf(writePos, maxArgStrLen - (writePos - argstr), " ");

            writePos += snprintf(writePos, maxArgStrLen - (writePos - argstr), "%s", argv[i]);
        }
    }

    u32 xmlBufferSize = BUFFER_SIZE + maxArgStrLen;
    char *xmlBuffer = (char*)malloc(xmlBufferSize);
    if(!xmlBuffer) {
        if(argstr)
            free(argstr);
        return;
    }

    char *writePos = xmlBuffer;

    //!----------------------------------------------------------------------------------------------------------------------------------------------------------------
    //! create the cos.xml
    //!----------------------------------------------------------------------------------------------------------------------------------------------------------------
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "<app type=\"complex\" access=\"777\">\n");
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <version type=\"unsignedInt\" length=\"4\">%i</version>\n", cosAppXmlInfoStruct.version_cos_xml);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <cmdFlags type=\"unsignedInt\" length=\"4\">%i</cmdFlags>\n", cosAppXmlInfoStruct.cmdFlags);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <argstr type=\"string\" length=\"4096\">%s</argstr>\n", argstr ? argstr : "");
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <avail_size type=\"hexBinary\" length=\"4\">%08X</avail_size>\n", cosAppXmlInfoStruct.avail_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <codegen_size type=\"hexBinary\" length=\"4\">%08X</codegen_size>\n", cosAppXmlInfoStruct.codegen_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <codegen_core type=\"hexBinary\" length=\"4\">%08X</codegen_core>\n", cosAppXmlInfoStruct.codegen_core);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <max_size type=\"hexBinary\" length=\"4\">%08X</max_size>\n", cosAppXmlInfoStruct.max_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <max_codesize type=\"hexBinary\" length=\"4\">%08X</max_codesize>\n", cosAppXmlInfoStruct.max_codesize);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <default_stack0_size type=\"hexBinary\" length=\"4\">%08X</default_stack0_size>\n", cosAppXmlInfoStruct.default_stack0_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <default_stack1_size type=\"hexBinary\" length=\"4\">%08X</default_stack1_size>\n", cosAppXmlInfoStruct.default_stack1_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <default_stack2_size type=\"hexBinary\" length=\"4\">%08X</default_stack2_size>\n", cosAppXmlInfoStruct.default_stack2_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <default_redzone0_size type=\"hexBinary\" length=\"4\">%08X</default_redzone0_size>\n", cosAppXmlInfoStruct.default_redzone0_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <default_redzone1_size type=\"hexBinary\" length=\"4\">%08X</default_redzone1_size>\n", cosAppXmlInfoStruct.default_redzone1_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <default_redzone2_size type=\"hexBinary\" length=\"4\">%08X</default_redzone2_size>\n", cosAppXmlInfoStruct.default_redzone2_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <exception_stack0_size type=\"hexBinary\" length=\"4\">%08X</exception_stack0_size>\n", cosAppXmlInfoStruct.exception_stack0_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <exception_stack1_size type=\"hexBinary\" length=\"4\">%08X</exception_stack1_size>\n", cosAppXmlInfoStruct.exception_stack1_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "  <exception_stack2_size type=\"hexBinary\" length=\"4\">%08X</exception_stack2_size>\n", cosAppXmlInfoStruct.exception_stack2_size);
    writePos += snprintf(writePos, xmlBufferSize - (writePos - xmlBuffer), "</app>");

    if(argstr)
    {
        free(argstr);
        argstr = NULL;
    }

    u32 length = writePos - xmlBuffer;

    sendData->tag = 0x02;
    memcpy(&sendData->data[0], &length, 4);
    sendData->length = snprintf((char*)sendData->data + 4, BUFFER_SIZE, "/vol/code/cos.xml") + 4 + 1;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

    int ret = recvwait(iClientSocket, (char*)sendData, sizeof(SendData) + 1);
    if(ret == (sizeof(SendData) + 1) && (sendData->data[0] == 1))
    {
        sendData->tag = 0x03;
        sendData->length = length;

        memcpy(sendData->data, xmlBuffer, length);

        if(sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length) < 0)
        {
            free(xmlBuffer);
            return;
        }

        sendData->tag = 0x04;
        sendData->length = 0;
        sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);
    }


    //!----------------------------------------------------------------------------------------------------------------------------------------------------------------
    //! create the app.xml
    //!----------------------------------------------------------------------------------------------------------------------------------------------------------------
    writePos = xmlBuffer;
    writePos += sprintf(writePos, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    writePos += sprintf(writePos, "<app type=\"complex\" access=\"777\">\n");
    writePos += sprintf(writePos, "  <version type=\"unsignedInt\" length=\"4\">%i</version>\n", 15);
    writePos += sprintf(writePos, "  <os_version type=\"hexBinary\" length=\"8\">%08X%08X</os_version>\n", (u32)(cosAppXmlInfoStruct.os_version >> 32), (u32)(cosAppXmlInfoStruct.os_version & 0xFFFFFFFF));
    writePos += sprintf(writePos, "  <title_id type=\"hexBinary\" length=\"8\">%08X%08X</title_id>\n", (u32)(cosAppXmlInfoStruct.title_id >> 32), (u32)(cosAppXmlInfoStruct.title_id & 0xFFFFFFFF));
    writePos += sprintf(writePos, "  <title_version type=\"hexBinary\" length=\"2\">%04X</title_version>\n", cosAppXmlInfoStruct.title_version);
    writePos += sprintf(writePos, "  <sdk_version type=\"unsignedInt\" length=\"4\">%i</sdk_version>\n", cosAppXmlInfoStruct.sdk_version);
    writePos += sprintf(writePos, "  <app_type type=\"hexBinary\" length=\"4\">%08X</app_type>\n", cosAppXmlInfoStruct.app_type);
    writePos += sprintf(writePos, "  <group_id type=\"hexBinary\" length=\"4\">%08X</group_id>\n", (u32)(cosAppXmlInfoStruct.title_id >> 8) & 0xFFFF);
    writePos += sprintf(writePos, "  <os_mask type=\"hexBinary\" length=\"32\">0000000000000000000000000000000000000000000000000000000000000000</os_mask>\n");
    writePos += sprintf(writePos, "</app>");

    length = writePos - xmlBuffer;

    sendData->tag = 0x02;
    memcpy(&sendData->data[0], &length, 4);
    sendData->length = snprintf((char*)sendData->data + 4, BUFFER_SIZE, "/vol/code/app.xml") + 4 + 1;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

    ret = recvwait(iClientSocket, (char*)sendData, sizeof(SendData) + 1);
    if(ret == (sizeof(SendData) + 1) && (sendData->data[0] == 1))
    {
        sendData->tag = 0x03;
        sendData->length = length;

        memcpy(sendData->data, xmlBuffer, length);

        if(sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length) < 0)
        {
            free(xmlBuffer);
            return;
        }

        sendData->tag = 0x04;
        sendData->length = 0;
        sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);
    }

    free(xmlBuffer);
}

void DumpMetaPath(void *pClient, void *pCmd, SendData *sendData)
{
    if(u64TitleMetaDataPending == 0)
        return;

    //! meta XML is a bit special
    //! we are in a completely different context on this one
    //! and therefor we need to start a new client session
    //! luckily this context is only called when the other one is already stopped
    //! so no mutex locks or other thread safety measurements are required
    int allocated = 0;
    char *pPath = NULL;
    int iOldClientSocket = iClientSocket;
    iClientSocket = -1;

    do
    {
        if(server_connect(&iClientSocket, serverIpAddress) < 0)
            break;

        if(!sendData)
        {
            sendData = (SendData*) memalign(0x20, ALIGN32(sizeof(SendData) + BUFFER_SIZE));
            if(!sendData)
                break;

            allocated = 1;
        }

        int ret = recvwait(iClientSocket, sendData, sizeof(SendData) + 1);
        if(ret != sizeof(SendData) + 1)
            break;

        unsigned int pathLen = sendData->length - 1;
        int expectedLength = (pathLen < BUFFER_SIZE) ? pathLen : BUFFER_SIZE;
        recvwait(iClientSocket, sendData->data, expectedLength);

        char *pPath = (char*) malloc(FS_MAX_FULLPATH_SIZE);
        if(!pPath)
            break;

        strcpy(pPath, "/vol/meta");
        int result = DumpDir(pClient, pCmd, sendData, pPath);
        //! if meta dumping actually worked from HOME menu, then we need to reset this so it wont dump again on exit to system menu
        if(result == 0)
            u64TitleMetaDataPending = 0;
    }
    while(0);

    if(pPath)
        free(pPath);

    if(allocated)
        free(sendData);

    if(iClientSocket != -1)
        socketclose(iClientSocket);

    iClientSocket = iOldClientSocket;
}

void DumpRpxRpl(SendData *sendData)
{
    if(rpxRplTableGetCount() == 0 || iClientSocket < 0 || !u8DumpingPaths)
        return;

    int allocated = 0;

    if(!sendData)
    {
        sendData = (SendData*)memalign(0x20, ALIGN32(sizeof(SendData) + BUFFER_SIZE));
        if(!sendData)
            return;

        allocated = 1;
    }

    sendData->tag = 0x01;
    sendData->length = snprintf((char*)sendData->data, BUFFER_SIZE, "/vol/code") + 1;
    sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);


    s_rpx_rpl *rpx_rpl_struct = (s_rpx_rpl*)(rpxRplTableGet());

    do
    {
        sendData->tag = 0x02;
        memcpy(&sendData->data[0], &rpx_rpl_struct->size, 4);
        sendData->length = snprintf((char*)sendData->data + 4, BUFFER_SIZE, "/vol/code/%s", rpx_rpl_struct->name) + 4 + 1;
        sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);

        int ret = recvwait(iClientSocket, (char*)sendData, sizeof(SendData) + 1);
        if(ret != (sizeof(SendData) + 1) || (sendData->data[0] != 1))
        {
            rpx_rpl_struct = rpx_rpl_struct->next;
            continue;
        }

        unsigned int data_pos = 0;
        unsigned char * sendDataPhys = (unsigned char*)OSEffectiveToPhysical(sendData->data);

        // invalidate cache so it wont get written during or after our physical write and overwrite the data
        InvalidateRange((u32)sendData->data, BUFFER_SIZE);

        while(data_pos < rpx_rpl_struct->size)
        {
            int blockSize = (data_pos + BUFFER_SIZE < rpx_rpl_struct->size) ? BUFFER_SIZE : rpx_rpl_struct->size - data_pos;

            int copiedBytes = rpxRplCopyDataFromMem(rpx_rpl_struct, data_pos, sendDataPhys, blockSize);
            if(copiedBytes <= 0)
                break;

            // invalidate cache to read new data directly from memory
            InvalidateRange((u32)sendData->data, copiedBytes);

            sendData->tag = 0x03;
            sendData->length = copiedBytes;
            if(sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length) < 0) {
                break;
            }

            data_pos += copiedBytes;
        }

        sendData->tag = 0x04;
        sendData->length = 0;
        sendwait(iClientSocket, sendData, sizeof(SendData) + sendData->length);
        rpx_rpl_struct = rpx_rpl_struct->next;
    }
    while(rpx_rpl_struct);

    rpxRplTableInit();

    if(allocated)
        free(sendData);
}

static void DumpMainDir(void *pClient, void *pCmd, SendData *sendData, char *pPath)
{
    if(strncasecmp(pPath, "/vol/code", strlen("/vol/code")) == 0)
    {
        u8DumpingPaths = 1;
        DumpCodeXmls(sendData);
        DumpRpxRpl(sendData);
    }
    else if(strncasecmp(pPath, "/vol/content", strlen("/vol/content")) == 0)
    {
        DumpDir(pClient, pCmd, sendData, pPath);
    }
    else if(strncasecmp(pPath, "/vol/aoc", strlen("/vol/aoc")) == 0)
    {
        if(AOC_Initialize == 0)
            return;

        s32 aocWasStarted = AOC_Initialize();

        u32 num_titles = 0;
        u32 buffer_size = AOC_CalculateWorkBufferSize(0x2000);

        unsigned char* title_list = (unsigned char*) memalign(0x40, 0x2000 * AOC_TITLE_SIZE);
        if(!title_list) {
            AOC_Finalize();
            return;
        }

        unsigned char* buffer = (unsigned char*) memalign(0x40, buffer_size);
        if(!buffer) {
            free(title_list);
            AOC_Finalize();
            return;
        }

        s32 ret = AOC_ListTitle(&num_titles, title_list, 0x2000, buffer, buffer_size);
        if (ret == 0)
        {
            u32 i;
            for (i = 0; i < num_titles; i++)
            {
                ret = AOC_OpenTitle(pPath, title_list + i * AOC_TITLE_SIZE, buffer, buffer_size);
                if(ret == 0)
                {
                    DumpDir(pClient, pCmd, sendData, pPath);
                    AOC_CloseTitle(title_list + i * AOC_TITLE_SIZE);
                }
            }
        }
        if(aocWasStarted == 0)
            AOC_Finalize();
        free(title_list);
        free(buffer);
    }
    else if(strncasecmp(pPath, "/vol/meta", strlen("/vol/meta")) == 0)
    {
        u64TitleMetaDataPending = OSGetTitleID();
    }
    else if(strncasecmp(pPath, "/vol/save", strlen("/vol/save")) == 0)
    {
        unsigned int nn_act_handle;
        unsigned long (*GetPersistentIdEx)(unsigned char);
        int (*GetSlotNo)(void);
        void (*nn_Initialize)(void);
        void (*nn_Finalize)(void);
        OSDynLoad_Acquire("nn_act.rpl", &nn_act_handle);
        OSDynLoad_FindExport(nn_act_handle, 0, "GetPersistentIdEx__Q2_2nn3actFUc", &GetPersistentIdEx);
        OSDynLoad_FindExport(nn_act_handle, 0, "GetSlotNo__Q2_2nn3actFv", &GetSlotNo);
        OSDynLoad_FindExport(nn_act_handle, 0, "Initialize__Q2_2nn3actFv", &nn_Initialize);
        OSDynLoad_FindExport(nn_act_handle, 0, "Finalize__Q2_2nn3actFv", &nn_Finalize);

        nn_Initialize();
        unsigned int slotNo = GetSlotNo();
        unsigned int persistentId = GetPersistentIdEx(slotNo);
        nn_Finalize();

        unsigned int save_handle;
        OSDynLoad_Acquire("nn_save.rpl", &save_handle);

        int (*SAVEInit)();
        int (*SAVEInitSaveDir)(unsigned char accountSlotNo);
        OSDynLoad_FindExport(save_handle, 0, "SAVEInit",  &SAVEInit);
        OSDynLoad_FindExport(save_handle, 0, "SAVEInitSaveDir",  &SAVEInitSaveDir);

        SAVEInit();
        SAVEInitSaveDir(slotNo);
        SAVEInitSaveDir(255);

        snprintf(pPath, FS_MAX_FULLPATH_SIZE, "/vol/save/common");
        DumpDir(pClient, pCmd, sendData, pPath);
        snprintf(pPath, FS_MAX_FULLPATH_SIZE, "/vol/save/%08X", persistentId);
        DumpDir(pClient, pCmd, sendData, pPath);
    }
    else
    {
        DumpDir(pClient, pCmd, sendData, pPath);
    }
}

void StartDumper()
{
    SendData *sendData = NULL;
    void *pClient = NULL;
    void *pCmd = NULL;
    char *pPath = NULL;

    u64TitleMetaDataPending = 0;
    u8TitleLocation = TITLE_LOCATION_ODD;

    int mcpHandle = MCP_Open();
    if(mcpHandle != 0)
    {
        unsigned char titleInfo[0x80];
        memset(titleInfo, 0, sizeof(titleInfo));

        MCP_GetOwnTitleInfo(mcpHandle, titleInfo);
        MCP_Close(mcpHandle);

        if(strncmp((char*)&titleInfo[0x56], "mlc", 3) == 0) {
            u8TitleLocation = TITLE_LOCATION_MLC;
        }
        else if(strncmp((char*)&titleInfo[0x56], "usb", 3) == 0) {
            u8TitleLocation = TITLE_LOCATION_USB;
        }
        else {
            u8TitleLocation = TITLE_LOCATION_ODD;
        }
    }

    do
    {
        sendData = (SendData*)memalign(0x20, ALIGN32(sizeof(SendData) + BUFFER_SIZE));
        if(!sendData)
            break;

        if(server_connect(&iClientSocket, serverIpAddress) < 0)
            break;

        int ret = recvwait(iClientSocket, sendData, sizeof(SendData) + 1);
        if(ret != sizeof(SendData) + 1)
            break;

        //bss.recursive = sendData->data[0];

        unsigned int pathLen = sendData->length - 1;
        int expectedLength = (pathLen < BUFFER_SIZE) ? pathLen : BUFFER_SIZE;
        int received = recvwait(iClientSocket, sendData->data, expectedLength);
        if(received != expectedLength)
            break;

        pClient = malloc(FS_CLIENT_SIZE);
        if (!pClient)
            break;

        pCmd = malloc(FS_CMD_BLOCK_SIZE);
        if (!pCmd)
            break;

        pPath = (char*) malloc(FS_MAX_FULLPATH_SIZE);
        if(!pPath)
            break;

        FSInit();
        FSInitCmdBlock(pCmd);
        FSAddClientEx(pClient, 0, -1);

        snprintf(pPath, FS_MAX_FULLPATH_SIZE, "%s", (char*) sendData->data);

        if(strcmp(pPath, "/vol") == 0 ||  strcmp(pPath, "/vol/") == 0)
        {
            strcpy(pPath, "/vol/code");
            DumpMainDir(pClient, pCmd, sendData, pPath);
            strcpy(pPath, "/vol/content");
            DumpMainDir(pClient, pCmd, sendData, pPath);
            strcpy(pPath, "/vol/meta");
            DumpMainDir(pClient, pCmd, sendData, pPath);
            strcpy(pPath, "/vol/save");
            DumpMainDir(pClient, pCmd, sendData, pPath);
            strcpy(pPath, "/vol/aoc");
            DumpMainDir(pClient, pCmd, sendData, pPath);
        }
        else
        {
            DumpMainDir(pClient, pCmd, sendData, pPath);
        }

        FSDelClient(pClient);
    }
    while(0);

    if(pCmd)
        free(pCmd);
    if(pClient)
        free(pClient);
    if(pPath)
        free(pPath);
    if(sendData)
        free(sendData);

    //! we dont close socket as stuff is also dumped dynamically during game play
    /*
    if(iClientSocket >= 0)
    {
        socketclose(iClientSocket)
        iClientSocket = -1;
    }
    */
}

void ResetDumper(void)
{
    iClientSocket = -1;
    u8DumpingPaths = 0;

    // reset RPX table on every launch of system menu
    rpxRplTableInit();
}


void SetServerIp(u32 ip)
{
    serverIpAddress = ip;
    int iFd = open(SERVER_IP_HEX_PATH, O_WRONLY);
	if (iFd >= 0)
	{
		write(iFd, (u8 *) &serverIpAddress, 4);
		close(iFd);
	}
}

u32 GetServerIp(void)
{
    int iFd = open(SERVER_IP_HEX_PATH, O_RDONLY);
	if (iFd >= 0)
	{
		read(iFd, (u8 *) &serverIpAddress, 4);
		close(iFd);
	}
	return serverIpAddress;
}

static int thread_callback(int argc, void *argv)
{
    //! this delay is required for the system menu to fire up all subsystems
    //! if its not done an error message is output about disc or usb failures (if attached)
    sleep(15);

    char *metaDir = NULL;
    void *pClient = NULL;
    void *pCmd = NULL;

    do
    {
        metaDir = (char*) malloc(FS_MAX_FULLPATH_SIZE);
        if(!metaDir)
            break;

        pClient = malloc(FS_CLIENT_SIZE);
        if(!pClient)
            break;

        pCmd = malloc(FS_CMD_BLOCK_SIZE);
        if(!pCmd)
            break;

        unsigned int acp_handle = 0;
        OSDynLoad_Acquire("nn_acp.rpl", &acp_handle);

        if(acp_handle == 0)
            break;

        s32 (* ACPInitialize)(void) = 0;
        s32 (* ACPGetTitleMetaDir)(u64 titleId, char *metaDir, int bufferSize) = 0;
        s32 (* ACPGetTitleMetaDirByDevice)(u64 titleId, char *metaDir, int bufferSize) = 0;
        OSDynLoad_FindExport(acp_handle, 0, "ACPInitialize", &ACPInitialize);
        OSDynLoad_FindExport(acp_handle, 0, "ACPGetTitleMetaDir", &ACPGetTitleMetaDir);
        OSDynLoad_FindExport(acp_handle, 0, "ACPGetTitleMetaDirByDevice", &ACPGetTitleMetaDirByDevice);

        if(ACPInitialize == 0)
            break;

        ACPInitialize();

        if(ACPGetTitleMetaDirByDevice(u64TitleMetaDataPending, metaDir, FS_MAX_FULLPATH_SIZE) != 0)
            ACPGetTitleMetaDir(u64TitleMetaDataPending, metaDir, FS_MAX_FULLPATH_SIZE);

        FSInitCmdBlock(pCmd);
        FSAddClientEx(pClient, 0, -1);

        int ret = FSBindMount(pClient, pCmd, metaDir, "/vol/meta", -1);

        if(ret == 0)
        {
            DumpMetaPath(pClient, pCmd, NULL);
            FSBindUnmount(pClient, pCmd, metaDir, -1);
        }

        FSDelClient(pClient);
    }
    while(0);

    if(metaDir)
        free(metaDir);
    if(pClient)
        free(pClient);
    if(pCmd)
        free(pCmd);
    u64TitleMetaDataPending = 0;

    return 0;
}

void CheckPendingMetaDump()
{
    if(u64TitleMetaDataPending == 0)
        return;

    //! allocate the thread
    void *pThread = memalign(8, 0x1000);
    //! allocate the stack
    void *pThreadStack = (u8 *) memalign(0x20, 0x2000);
    OSCreateThread(pThread, &thread_callback, 0, 0, (u32)pThreadStack+0x2000, 0x2000, 0, 0x1A);
    OSResumeThread(pThread);
}

int IsDumpingDiscUsbMeta(void)
{
    return u64TitleMetaDataPending && (u8TitleLocation != TITLE_LOCATION_MLC);
}
