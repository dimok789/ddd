#ifndef _DISCDUMPER_H_
#define _DISCDUMPER_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned char tag;
    unsigned int length;
    unsigned char data[0];
} __attribute__((packed)) SendData;

void DumpRpxRpl(SendData *sendData);
void DumpMetaPath(void *pClient, void *pCmd, SendData *sendData);
void CheckPendingMetaDump(void);
void StartDumper(void);
void ResetDumper(void);

void SetServerIp(u32 ip);
u32 GetServerIp(void);

int IsDumpingDiscUsbMeta(void);

#define SERVER_IP_HEX_PATH       "sd:/wiiu/apps/ddd/serverIpAddress.hex"

#ifdef __cplusplus
}
#endif

#endif
