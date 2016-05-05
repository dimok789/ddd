/****************************************************************************
 * Copyright (C) 2010-2016
 * by Dimok
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you
 * must not claim that you wrote the original software. If you use
 * this software in a product, an acknowledgment in the product
 * documentation would be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and
 * must not be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source
 * distribution.
 *****************************************************d**********************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <sys/stat.h>
#include "Input.h"
#include "network.h"

typedef struct {
    unsigned char tag;
    unsigned int length;
    unsigned char data[0];
} __attribute__((packed)) SendData;

static const char *cpOutputPath = NULL;

#define le16(i) ((((unsigned short) ((i) & 0xFF)) << 8) | ((unsigned short) (((i) & 0xFF00) >> 8)))
#define le32(i) ((((unsigned int)le16((i) & 0xFFFF)) << 16) | ((unsigned int)le16(((i) & 0xFFFF0000) >> 16)))

unsigned long long gettime()
{
#ifdef WIN32
    return GetTickCount();
#else
	struct timeval currentTime;
	gettimeofday(&currentTime, NULL);
	return (currentTime.tv_sec * 1000 + currentTime.tv_usec / 1000);
#endif
}

int CreateSubfolder(const char * fullpath)
{
	if(!fullpath)
		return 0;

	//! make copy of string
	int length = strlen(fullpath);

	char *dirpath = (char *) malloc(length+2);
	if(!dirpath)
		return 0;

	strcpy(dirpath, fullpath);

	//! remove unnecessary slashes
	while(length > 0 && dirpath[length-1] == '/')
		--length;

	dirpath[length] = '\0';

	//! if its the root then add one slash
	char * notRoot = strrchr(dirpath, '/');
	if(!notRoot && ((strlen(dirpath) == 0) || (dirpath[strlen(dirpath) - 1] == ':')))
		strcat(dirpath, "/");

	int ret;
	//! clear stack when done as this is recursive
	{
		struct stat filestat;
		ret = stat(dirpath, &filestat);
	}

	//! entry found
	if(ret == 0)
	{
		free(dirpath);
		return 1;
	}
	//! if its root and stat failed the device doesnt exist
	else if(!notRoot)
	{
        int result = 0;
	    //! if we are at the front of the path and its relative without a dot
	    if(strlen(dirpath) > 0 && dirpath[strlen(dirpath) - 1] != ':')
        {
            //! try creating the directory now
#ifdef WIN32
            result = (mkdir(dirpath) == 0);
#else
            result = (mkdir(dirpath, 0777) == 0);
#endif
        }
		free(dirpath);
		return result;
	}

	//! cut to previous slash and do a recursion
	*notRoot = '\0';

	int result = 0;

	if(CreateSubfolder(dirpath))
	{
		//! the directory inside which we create the new directory exists, so its ok to create
		//! add back the slash for directory creation
		*notRoot = '/';
		//! try creating the directory now
#ifdef WIN32
		result = (mkdir(dirpath) == 0);
#else
		result = (mkdir(dirpath, 0777) == 0);
#endif
	}

	free(dirpath);

	return result;
}

static FILE *pFile = NULL;
static unsigned int lastSize = 0;
static unsigned int lastTime = 0;

void processTag(int client_socket, SendData *sendData)
{
    switch(sendData->tag)
    {
    case 0x01: {
        char *ptr = (char*)malloc(strlen(cpOutputPath) + strlen((char*)sendData->data) + 1);
        sprintf(ptr, "%s%s", cpOutputPath, (char*)sendData->data);
        printf("Create path: %s\n", ptr);
        CreateSubfolder(ptr);
        free(ptr);
        break;
    }
    case 0x02: {
        unsigned int fileSize;
        memcpy(&fileSize, sendData->data, 4);
        fileSize = le32(fileSize);

        char *filePath = (char*)sendData->data+4;
        char *localPath = (char*)malloc(strlen(cpOutputPath) + strlen(filePath) + 1);
        sprintf(localPath, "%s%s", cpOutputPath, filePath);

        //! check if last file was closed properly
        if(pFile) {
            printf("Previous file was not closed pFile != NULL\n");
            fclose(pFile);
        }

        int confirmSend = 1;

        //! check filesize of existing files
		struct stat filestat;
        if(stat(localPath, &filestat) == 0)
            confirmSend = (filestat.st_size != fileSize);

        sendData->data[0] = confirmSend;
        sendData->length = le32(1);
        NetWrite(client_socket, sendData, sizeof(SendData) + le32(sendData->length));

        if(confirmSend == 0)
        {
            printf("File with same size exists, skipping: %s (%i kb)\n", localPath, fileSize/1024);
        }
        else
        {
            lastTime = gettime();
            lastSize = 0;
            pFile = fopen(localPath, "wb");
            printf("Open file: %s\n", localPath);
            if(!pFile) {
                printf("Failed to open: %s\n", localPath);
            }
        }
        free(localPath);
        break;
    }
    case 0x03: {
        if(!pFile) {
            break;
        }
        unsigned long int size = ftell(pFile);
        unsigned int time = gettime();
        float fTimeDiff = (time - lastTime) * 0.001f;
        float fSpeed = (fTimeDiff == 0.0f) ? 0.0f : ( (float)size / fTimeDiff / 1024.0f );
        printf("Read file %lu kb loaded with %0.3f kb/s\r", size / 1024, fSpeed);
        fwrite(sendData->data, 1, le32(sendData->length), pFile);
        break;
    }
    case 0x04: {
        if(!pFile) {
            printf("pFile == NULL on close\n");
            break;
        }
        printf("\n");
        fclose(pFile);
        pFile = NULL;
        break;
    }
    default:
        printf("Unknown TAG %08X\n", sendData->tag);
        break;
    }
}

#define MAX_CLIENTS 5

int main(int argc, char *argv[])
{
    printf("Title Dumper by Dimok\n");
    if(argc < 3) {
        printf("Usage:\n");
        printf("titledumper [WiiU Path] [Local Path]\n");
        printf("WiiU Path supported: /vol (for complete dump), /vol/content, /vol/code, /vol/meta, /vol/aoc and /vol/save or any sub-directories of those)\n");
        printf("Local Path e.g.: D:/some game/path or ./some game/path\n");
        return 0;
    }
    printf("Waiting for WiiU connection...\n");

    char *ptr = (char*)argv[1];
    while(*ptr)
    {
        if(*ptr == '\\')
            *ptr = '/';
        ptr++;
    }


    ptr = (char*)argv[2];
    while(*ptr)
    {
        if(*ptr == '\\')
            *ptr = '/';
        ptr++;
    }

    while(argv[1][strlen(argv[1])-1] == '/')
        argv[1][strlen(argv[1])-1] = '\0';
    while(argv[2][strlen(argv[2])-1] == '/')
        argv[2][strlen(argv[2])-1] = '\0';

    const char *cpTargetPath = argv[1];
    cpOutputPath = argv[2];

    int serverSocket = NetInit();
    if(serverSocket < 0)
    {
        printf("Can't open socket.\n");
        usleep(999999);
        usleep(999999);
        return 0;
    }
    if(Bind() < 0)
    {
        printf("Can't bind socket.\n");
        usleep(999999);
        usleep(999999);
        return 0;
    }

	int ret;
	const int blockSize = 0x20000;
	unsigned char *data = (unsigned char*)malloc(blockSize);
    SendData *sendData = (SendData *)data;

    int length = 0;

    int i;
    int clientSockets[MAX_CLIENTS];
    for(i = 0; i < MAX_CLIENTS; i++)
        clientSockets[i] = -1;

	fd_set fdReadSet;
    struct timeval timeout;

    while(1)
    {
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int iMaxFd = serverSocket;
        FD_ZERO(&fdReadSet);
        FD_SET(serverSocket, &fdReadSet);

        for(i = 0; i < MAX_CLIENTS; ++i)
        {
            if(clientSockets[i] == -1)
                continue;

            if(iMaxFd < clientSockets[i])
                iMaxFd = clientSockets[i];
            FD_SET(clientSockets[i], &fdReadSet);
        }

        int sel_ret = select(iMaxFd + 1, &fdReadSet, NULL, NULL, &timeout);
        if(sel_ret == 0)
            continue;

		if(FD_ISSET(serverSocket, &fdReadSet))
		{
            int clientSock = Accept();
            if(clientSock >= 0)
            {
                int found = 0;
                for(i = 0; i < MAX_CLIENTS; i++)
                {
                    if(clientSockets[i] == -1)
                    {
                        printf("Client %i connected\n", i);
                        clientSockets[i] = clientSock;
                        found = 1;
                        break;
                    }
                }

                if(found)
                {
                    unsigned int pathLen = strlen(cpTargetPath) + 1;
                    sendData->tag = 0x00;
                    sendData->length = le32(1 + pathLen);
                    sendData->data[0] = 1; // recursive
                    memcpy(&sendData->data[1], cpTargetPath, pathLen);
                    NetWrite(clientSock, sendData, sizeof(SendData) + le32(sendData->length));
                }
                else
                    close(clientSock);
            }
		}

        for(i = 0; i < MAX_CLIENTS; ++i)
        {
            if(clientSockets[i] == -1)
                continue;

            if(FD_ISSET(clientSockets[i], &fdReadSet))
            {
                ret = NetRead(clientSockets[i], data + length, blockSize - length);

                if(ret > 0)
                {
                    length += ret;

                    while((length >= sizeof(SendData)) && (length >= (sizeof(SendData) + le32(sendData->length))))
                    {
                        int processed = le32(sendData->length) + sizeof(SendData);

                        processTag(clientSockets[i], sendData);

                        length -= processed;
                        memcpy(data, data + processed, length);
                    }
                }
                else if(ret <= 0)
                {
                    printf("Client %i connection closed\n", i);
                    close(clientSockets[i]);
                    clientSockets[i] = -1;
                }
            }
        }
    }

    CloseSocket();
    free(data);

    return 0;
}
