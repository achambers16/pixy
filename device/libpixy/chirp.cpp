#include <string.h>
#include "chirp.hpp"

// todo yield, sleep() while waiting for sync response
// todo

#define ALIGN(v, n)  v = v&((n)-1) ? (v&~((n)-1))+(n) : v

// assume that destination is aligned on the correct boundary and copy the source byte by byte
void copyAlign(char *dest, const char *src, int size)
{
    int i;
    for (i=0; i<size; i++)
        dest[i] = src[i];
}

Chirp::Chirp(bool hinterested, Link *link)
{
    m_link = NULL;
    m_errorCorrected = false;
    m_sharedMem = false;
    m_buf = NULL;
    m_buf2 = NULL;
    m_preBuf = 0;

    m_maxNak = CRP_MAX_NAK;
    m_retries = CRP_RETRIES;
    m_headerTimeout = CRP_HEADER_TIMEOUT;
    m_dataTimeout = CRP_DATA_TIMEOUT;
    m_idleTimeout = CRP_IDLE_TIMEOUT;
    m_sendTimeout = CRP_SEND_TIMEOUT;
    m_remoteInit = false;
    m_connected = false;
    m_hinformer = false;
    m_hinterested = hinterested;

    m_procTableSize = CRP_PROCTABLE_LEN;
    m_procTable = new ProcTableEntry[m_procTableSize];
    memset(m_procTable, 0, sizeof(ProcTableEntry)*m_procTableSize);

    if (link)
        setLink(link);
}

Chirp::~Chirp()
{
    if (!m_sharedMem)
        delete[] m_buf;
    delete[] m_procTable;
}

int Chirp::init()
{
    return CRP_RES_OK;
}

void Chirp::setLink(Link *link)
{
    m_link = link;
    m_errorCorrected = m_link->getFlags()&LINK_FLAG_ERROR_CORRECTED;
    m_sharedMem = m_link->getFlags()&LINK_FLAG_SHARED_MEM;
    m_blkSize = m_link->blockSize();

    if (m_errorCorrected)
        m_headerLen = 12; // startcode (uint32_t), type (uint8_t), (pad), proc (uint16_t), len (uint32_t)
    else
        m_headerLen = 8;  // type (uint8_t), (pad), proc (uint16_t), len (uint32_t)

    if (m_sharedMem)
    {
        m_buf = (uint8_t *)m_link->getFlags(LINK_FLAG_INDEX_SHARED_MEMORY_LOCATION);
        m_bufSize = m_link->getFlags(LINK_FLAG_INDEX_SHARED_MEMORY_SIZE);
    }
    else
    {
        m_bufSize = CRP_BUFSIZE;
        m_buf = new uint8_t[m_bufSize];
    }

    // link is set up, need to call init
    init();
}


int Chirp::assemble(int dummy, ...)
{
    int res;
    va_list args;

    va_start(args, dummy);
    res = assembleHelper(&args);
    va_end(args);

    return res;
}

int Chirp::assembleHelper(va_list *args)
{
    int res;
    uint8_t type, origType;
    uint32_t i, si;

    // restore buffer if we use CRP_USE_BUFFER
    if (m_buf2)
    {
        m_buf = m_buf2;
        m_bufSize = m_bufSize2;
        m_buf2 = NULL;
    }

    for (i=m_headerLen+m_len; true;)
    {
#if defined(__WIN32__) || defined(__arm)
        type = va_arg(*args, int);
#else
        type = va_arg(*args, uint8_t);
#endif

        if (type==END)
            break;
        else if (type==CRP_USE_BUFFER && !m_sharedMem)
        {   // save buffer
            m_buf2 = m_buf;
            m_bufSize2 = m_bufSize;
            m_bufFlag = false;
            // set new buffer
            m_bufSize = va_arg(*args, uint32_t);
            m_buf = va_arg(*args, uint8_t *);
            continue;
        }

        si = i; // save index so we can skip over data if needed
        m_buf[i++] = type;

        // treat hints like other types for now
        // but if gotoe isn't interested  in hints (m_hinformer=false),
        // we'll restore index to si and effectively skip data.
        origType = type;
        type &= ~CRP_HINT;

        if (type==CRP_INT8)
        {
#if defined(__WIN32__) || defined(__arm)
            int8_t val = va_arg(*args, int);
#else
            int8_t val = va_arg(*args, int8_t);
#endif
            *(int8_t *)(m_buf+i) = val;
            i += 1;
        }
        else if (type==CRP_INT16)
        {
#if defined(__WIN32__) || defined(__arm)
            int16_t val = va_arg(*args, int);
#else
            int16_t val = va_arg(*args, int16_t);
#endif
            ALIGN(i, 2);
            // rewrite type so getType will work (even though we might add padding between type and data)
            m_buf[i-1] = origType;
            *(int16_t *)(m_buf+i) = val;
            i += 2;
        }
        else if (type==CRP_INT32 || origType==CRP_TYPE_HINT) // CRP_TYPE_HINT is a special case...
        {
            int32_t val = va_arg(*args, int32_t);
            ALIGN(i, 4);
            m_buf[i-1] = origType;
            *(int32_t *)(m_buf+i) = val;
            i += 4;
        }
        else if (type==CRP_FLT32)
        {
#if defined(__WIN32__) || defined(__arm)
            float val = va_arg(*args, double);
#else
            float val = va_arg(*args, float);
#endif
            ALIGN(i, 4);
            m_buf[i-1] = origType;
            *(float *)(m_buf+i) = val;
            i += 4;
        }
        else if (type==CRP_STRING)
        {
            int8_t *s = va_arg(*args, int8_t *);
            uint32_t len = strlen((char *)s)+1; // include null

            if (len+i > m_bufSize-CRP_BUFPAD && (res=realloc(len+i))<0)
                return res;

            memcpy(m_buf+i, s, len);
            i += len;
        }
        else if (type&CRP_ARRAY)
        {
            uint8_t size = type&0x0f;
            uint32_t len = va_arg(*args, int32_t);

            ALIGN(i, 4);
            m_buf[i-1] = origType;
            *(uint32_t *)(m_buf+i) = len;
            i += 4;
            ALIGN(i, size);
            len *= size; // scale by size of array elements

            if (len+i>m_bufSize-CRP_BUFPAD && (res=realloc(len+i))<0)
                return res;

            int8_t *ptr = va_arg(*args, int8_t *);
            if (m_buf2==NULL || m_bufFlag) // normal buffer, do copy
                memcpy(m_buf+i, ptr, len);
            else if (m_buf+i != (uint8_t *)ptr)	// otherwise check pointer
            {
                m_preBuf = i;
                return CRP_RES_ERROR_PARSE;
            }
            // m_bufFlag and USE_BUFFER set means we copy remaining data.
            m_bufFlag = true;
            i += len;
        }
        else
            return CRP_RES_ERROR_PARSE;

        // skip hint data if we're not a source
        if (!m_hinformer && origType&CRP_HINT)
            i = si;

        if (i>m_bufSize-CRP_BUFPAD && (res=realloc())<0)
            return res;
    }

    // set length
    m_len = i-m_headerLen;

    return CRP_RES_OK;
}

// this isn't completely necessary, but it makes things a lot easier to use.
// passing a pointer to a pointer and then having to dereference is just confusing....
// so for scalars (ints, floats) you don't need to pass in ** pointers, just * pointers so
// chirp can write the value into the * pointer and hand it back.
// But for arrays you need ** pointers, so chirp doesn't need to copy the whole array into your buffer---
// chirp will write the * pointer value into your ** pointer.
int Chirp::loadArgs(va_list *args, void *recvArgs[])
{
    int i;
    uint8_t type, size;
    void **recvArg;

    for (i=0; recvArgs[i]!=NULL && i<CRP_MAX_ARGS; i++)
    {
        type = getType(recvArgs[i]);
        recvArg = va_arg(*args, void **);
        if (recvArg==NULL)
            return CRP_RES_ERROR_PARSE;

        if (!(type&CRP_ARRAY)) // if we're a scalar
        {
            size = type&0x0f;
            if (size==1) *(uint8_t *)recvArg = *(uint8_t *)recvArgs[i];
            else if (size==2) *(uint16_t *)recvArg = *(uint16_t *)recvArgs[i];
            else if (size==4) *(uint32_t *)recvArg = *(uint32_t *)recvArgs[i];
            //else if (size==8) *recvArg = *(double *)recvArgs[i];
            else return CRP_RES_ERROR_PARSE;
        }
        else // we're an array
        {
            if (type==CRP_STRING)
                *(char **)recvArg = (char *)recvArgs[i];
            else
            {
                *(uint32_t *)recvArg = *(uint32_t *)recvArgs[i++];
                recvArg = va_arg(*args, void **);
                if (recvArg==NULL)
                    return CRP_RES_ERROR_PARSE;
                *(void **)recvArg = recvArgs[i];
            }
        }
    }
    // check to see if last arg is NULL, if not, we have a parse error
    // if the arg isn't null, it means the caller is expecting data to be
    // put there.  If data isn't put there, and the caller dereferences, segfault
    if (va_arg(*args, void **)!=NULL)
        return CRP_RES_ERROR_PARSE;

    return CRP_RES_OK;
}

int Chirp::call(uint8_t service, ChirpProc proc, ...)
{
    int res;
    uint8_t type;
    va_list args;

    // if it's just a regular call (not init or enumerate), we need to be connected
    if (!(service&CRP_CALL) && !m_connected)
        return CRP_RES_ERROR_NOT_CONNECTED;

    // parse args and assemble in m_buf
    va_start(args, proc);
    m_len = 0;
    if ((res=assembleHelper(&args))<0)
    {
        va_end(args);
        return res;
    }

    if (service&CRP_CALL) // special case for enumerate and init (internal calls)
    {
        type = service;
        service = SYNC;
    }
    else
        type = CRP_CALL;

    // send call data
    if ((res=sendChirpRetry(type, proc))!=CRP_RES_OK) // convert call into response
    {
        va_end(args);
        return res;
    }

    // if the service is synchronous, receive response while servicing other calls
    if (service==SYNC)
    {
        ChirpProc recvProc;
        void *recvArgs[CRP_MAX_ARGS+1];

        while(1)
        {
            if ((res=recvChirp(&type, &recvProc, recvArgs, true))==CRP_RES_OK)
            {
                if (type&CRP_RESPONSE)
                    break;
                else // handle calls as they come in
                    handleChirp(type, recvProc, recvArgs);
            }
            else
            {
                va_end(args);
                return res;
            }
        }

        // load args
        if ((res=loadArgs(&args, recvArgs))<0)
        {
            va_end(args);
            return res;
        }
    }


    va_end(args);
    return CRP_RES_OK;
}

int Chirp::sendChirpRetry(uint8_t type, ChirpProc proc)
{
    int i, res;

    for (i=0; i<m_retries; i++)
    {
        res = sendChirp(type, proc);
        if (res==CRP_RES_OK)
            break;
    }

    // restore buffer if we use CRP_USE_BUFFER
    if (m_buf2)
    {
        m_buf = m_buf2;
        m_bufSize = m_bufSize2;
        m_buf2 = NULL;
    }

    // if sending the chirp fails after retries, we should assume we're no longer connected
    if (res<0)
        m_connected = false;

    return res;
}

int Chirp::sendChirp(uint8_t type, ChirpProc proc)
{
    int res;
    if (m_errorCorrected)
        res = sendFull(type, proc);
    else
    {
        // we'll send forever as long as we get naks
        // we rely on receiver to give up
        while((res=sendHeader(type, proc))==CRP_RES_ERROR_CRC);
        if (res!=CRP_RES_OK)
            return res;
        res = sendData();
    }
    if (res!=CRP_RES_OK)
        return res;
    return CRP_RES_OK;
}

int Chirp::handleChirp(uint8_t type, ChirpProc proc, void *args[])
{
    int res;
    uint32_t responseInt = 0;
    uint8_t n;

    // reset data in case there is a null response
    m_len = 4; // leave room for responseInt

    // check for intrinsic calls
    if (type&CRP_INTRINSIC)
    {
        if (type==CRP_CALL_ENUMERATE)
            responseInt = handleEnumerate((char *)args[0], (ChirpProc *)args[1]);
        else if (type==CRP_CALL_INIT)
            responseInt = handleInit((uint16_t *)args[0], (uint8_t *)args[1]);
        else if (type==CRP_CALL_ENUMERATE_INFO)
            responseInt = handleEnumerateInfo((ChirpProc *)args[0]);
        else
            return CRP_RES_ERROR;
    }
    else // normal call
    {
        if (proc>=m_procTableSize)
            return CRP_RES_ERROR; // index exceeded

        ProcPtr ptr = m_procTable[proc].procPtr;
        if (ptr==NULL)
            return CRP_RES_ERROR; // some chirps are not meant to be called in both directions

        // count args
        for (n=0; args[n]!=NULL; n++);

        if (n==0)
            responseInt = (*ptr)(this);
        else if (n==1)
            responseInt = (*(uint32_t(*)(void*,Chirp*))ptr)(args[0],this);
        else if (n==2)
            responseInt = (*(uint32_t(*)(void*,void*,Chirp*))ptr)(args[0],args[1],this);
        else if (n==3)
            responseInt = (*(uint32_t(*)(void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],this);
        else if (n==4)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],this);
        else if (n==5)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],args[4],this);
        else if (n==6)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],args[4],args[5],this);
        else if (n==7)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],args[4],args[5],args[6],this);
        else if (n==8)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7],this);
        else if (n==9)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,void*,void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7],args[8],this);
        else if (n==10)
            responseInt = (*(uint32_t(*)(void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,Chirp*))ptr)(args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7],args[8],args[9],this);
    }

    // if it's a chirp call, we need to send back the result
    // result is in m_buf
    if (type&CRP_CALL)
    {
        // write responseInt
        *(uint32_t *)(m_buf+m_headerLen) = responseInt;
        // send response
        if ((res=sendChirpRetry(CRP_RESPONSE | (type&~CRP_CALL), m_procTable[proc].chirpProc))!=CRP_RES_OK) // convert call into response
            return res;
    }

    return CRP_RES_OK;
}

int Chirp::reallocTable()
{
    ProcTableEntry *newProcTable;
    int newProcTableSize;

    // allocate new table, zero
    newProcTableSize = m_procTableSize+CRP_PROCTABLE_LEN;
    newProcTable = new ProcTableEntry[newProcTableSize];
    memset(newProcTable, 0, sizeof(ProcTableEntry)*newProcTableSize);
    // copy to new table
    memcpy(newProcTable, m_procTable, sizeof(ProcTableEntry)*m_procTableSize);
    // delete old table
    delete [] m_procTable;
    // set to new
    m_procTable = newProcTable;
    m_procTableSize = newProcTableSize;

    return CRP_RES_OK;
}

ChirpProc Chirp::lookupTable(const char *procName)
{
    ChirpProc i;

    for(i=0; i<m_procTableSize; i++)
    {
        if (m_procTable[i].procName!=NULL && strcmp(m_procTable[i].procName, procName)==0)
            return i;
    }
    return -1;
}


ChirpProc Chirp::updateTable(const char *procName, ProcPtr procPtr)
{
    // if it exists already, update,
    // if it doesn't exist, add it
    if (procName==NULL)
        return -1;

    ChirpProc proc = lookupTable(procName);
    if (proc<0) // next empty entry
    {
        for (proc=0; proc<m_procTableSize && m_procTable[proc].procName; proc++);
        if (proc==m_procTableSize)
        {
            reallocTable();
            return updateTable(procName, procPtr);
        }
    }

    // add to table
    m_procTable[proc].procName = procName;
    m_procTable[proc].procPtr = procPtr;

    return proc;
}

ChirpProc Chirp::getProc(const char *procName, ProcPtr callback)
{
    uint32_t res;
    ChirpProc cproc = -1;

    if (callback)
        cproc = updateTable(procName, callback);

    if (call(CRP_CALL_ENUMERATE, 0,
             STRING(procName), // send name
             INT16(cproc), // send local index
             END_OUT_ARGS,
             &res, // get remote index
             END_IN_ARGS
             )>=0)
        return res;

    // a negative ChirpProc is an error
    return -1;
}

int Chirp::remoteInit()
{
    int res;
    uint32_t responseInt;
    uint8_t hinformer;

    // if we're being called remotely, don't call back
    if (m_remoteInit)
        return CRP_RES_OK;

    res = call(CRP_CALL_INIT, 0,
               UINT16(m_blkSize), // send block size
               UINT8(m_hinterested), // send whether we're interested in hints or not
               END_OUT_ARGS,
               &responseInt,
               &hinformer,       // receive whether we should send hints
               END_IN_ARGS
               );
    if (res>=0)
    {
        m_connected = true;
        m_hinformer = hinformer;
        return responseInt;
    }
    return res;
}

int Chirp::getProcInfo(ChirpProc proc, ProcInfo *info)
{
    uint32_t responseInt;
    int res;

    res = call(CRP_CALL_ENUMERATE_INFO, 0,
               UINT16(proc),
               END_OUT_ARGS,
               &responseInt,
               &info->procName,
               &info->argTypes,
               &info->procInfo,
               END_IN_ARGS
               );

    if (res==CRP_RES_OK)
        return responseInt;
    return res;
}

int Chirp::setProc(const char *procName, ProcPtr proc, ProcTableExtension *extension)
{
    ChirpProc cProc = updateTable(procName, proc);
    if (cProc<0)
        return CRP_RES_ERROR;

    m_procTable[cProc].extension = extension;
    return CRP_RES_OK;
}

int Chirp::registerModule(const ProcModule *module)
{
    int i;

    for (i=0; module[i].procName; i++)
        setProc(module[i].procName, module[i].procPtr, (ProcTableExtension *)module[i].argTypes);

    return CRP_RES_OK;
}

int32_t Chirp::handleEnumerate(char *procName, ChirpProc *callback)
{
    ChirpProc proc;
    // lookup in table
    proc = lookupTable(procName);
    // set remote index in table
    m_procTable[proc].chirpProc = *callback;

    return proc;
}

int32_t Chirp::handleInit(uint16_t *blkSize, uint8_t *hinformer)
{
    int32_t responseInt;

    m_remoteInit = true;
    responseInt = init();
    m_remoteInit = false;
    m_connected = true;
    m_blkSize = *blkSize;  // get block size, write it
    m_hinformer = *hinformer;

    CRP_RETURN(this, UINT8(m_hinterested), END);

    return responseInt;
}

int32_t Chirp::handleEnumerateInfo(ChirpProc *proc)
{
    const ProcTableExtension *extension;
    uint8_t null = '\0';

    if (*proc>=m_procTableSize || m_procTable[*proc].procName==NULL)
        extension = NULL;
    else
        extension = m_procTable[*proc].extension;

    if (extension)
    {
        CRP_RETURN(this, STRING(m_procTable[*proc].procName), STRING(extension->argTypes),
                   STRING(extension->procInfo), END);
        return CRP_RES_OK;
    }
    else // no extension, just send procedure name
    {
        CRP_RETURN(this, STRING(m_procTable[*proc].procName), STRING(&null),
                   STRING(&null), END);
        return CRP_RES_ERROR;
    }
}

int Chirp::realloc(uint32_t min)
{
    if (m_sharedMem || m_buf2!=NULL)
        return CRP_RES_ERROR_MEMORY;

    if (!min)
        min = m_bufSize+CRP_BUFSIZE;
    else
        min += CRP_BUFSIZE;
    uint8_t *newbuf = new uint8_t[min];
    memcpy(newbuf, m_buf, m_bufSize);
    delete[] m_buf;
    m_buf = newbuf;
    m_bufSize = min;

    return CRP_RES_OK;
}

// service deals with calls and callbacks
int Chirp::service()
{
    int i = 0;
    uint8_t type;
    ChirpProc recvProc;
    void *args[CRP_MAX_ARGS+1];

    while(recvChirp(&type, &recvProc, args)==CRP_RES_OK)
    {
        handleChirp(type, recvProc, args);
        i++;
    }

    return i;
}

int Chirp::recvChirp(uint8_t *type, ChirpProc *proc, void *args[], bool wait) // null pointer terminates
{
    int res;
    uint8_t dataType, size, a;
    uint32_t i;

    // receive
    if (m_errorCorrected)
        res = recvFull(type, proc, wait);
    else
    {
        for (i=0; true; i++)
        {
            res = recvHeader(type, proc, wait);
            if (res==CRP_RES_ERROR_CRC)
            {
                if (i<m_maxNak)
                    continue;
                else
                    return CRP_RES_ERROR_MAX_NAK;
            }
            else if (res==CRP_RES_OK)
                break;
            else
                return res;
        }
        res = recvData();
    }
    if (res!=CRP_RES_OK)
        return res;

    // get responseInt from response
    if (*type&CRP_RESPONSE)
    {
        // add responseInt to arg list
        args[0] = (void *)(m_buf+m_headerLen);
        *(m_buf+m_headerLen-1) = CRP_UINT32; // write type so it parses correctly
        // increment pointer
        i = m_headerLen+4;
        a = 1;
    }
    else // call has no responseInt
    {
        i = m_headerLen;
        a = 0;
    }
    // parse remaining args
    for(; i<m_len+m_headerLen; a++)
    {
        if (a==CRP_MAX_ARGS)
            return CRP_RES_ERROR;

        dataType = m_buf[i++];
        size = dataType&0x0f;
        if (!(dataType&CRP_ARRAY)) // if we're a scalar
        {
            ALIGN(i, size);
            args[a] = (void *)(m_buf+i);
            i += dataType&0x0f; // extract size of scalar, add it
        }
        else // we're an array
        {
            if (dataType==CRP_STRING) // string is a special case
            {
                args[a] = (void *)(m_buf+i);
                i += strlen((char *)(m_buf+i))+1; // +1 include null character
            }
            else
            {
                ALIGN(i, 4);
                uint32_t len = *(uint32_t *)(m_buf+i);
                args[a++] = (void *)(m_buf+i);
                i += 4;
                ALIGN(i, size);
                args[a] = (void *)(m_buf+i);
                i += len*size;
            }
        }
    }
    args[a] = NULL; // terminate list

    return CRP_RES_OK;
}

uint32_t Chirp::getPreBufLen()
{
    return m_preBuf;
}

uint8_t Chirp::getType(void *arg)
{
    return *((uint8_t *)arg - 1);
}

uint16_t Chirp::calcCrc(uint8_t *buf, uint32_t len)
{
    uint32_t i;
    uint16_t crc;

    // this isn't a real crc, but it's cheap and prob good enough
    for (i=0, crc=0; i<len; i++)
        crc += buf[i];
    crc += len;

    return crc;
}


int Chirp::sendFull(uint8_t type, ChirpProc proc)
{
    int res;

    *(uint32_t *)m_buf = CRP_START_CODE;
    *(uint8_t *)(m_buf+4) = type;
    *(ChirpProc *)(m_buf+6) = proc;
    *(uint32_t *)(m_buf+8) = m_len;
    // send header
    if ((res=m_link->send(m_buf, CRP_MAX_HEADER_LEN, m_sendTimeout))<0)
        return res;
    // if we haven't sent everything yet....
    if (m_len+m_headerLen>CRP_MAX_HEADER_LEN && !m_sharedMem)
    {
        if ((res=m_link->send(m_buf+CRP_MAX_HEADER_LEN, m_len-(CRP_MAX_HEADER_LEN-m_headerLen), m_sendTimeout))<0)
            return res;
    }
    return CRP_RES_OK;
}

int Chirp::sendHeader(uint8_t type, ChirpProc proc)
{
    int res;
    bool ack;
    uint32_t chunk, startCode = CRP_START_CODE;
    uint16_t crc;

    if ((res=m_link->send((uint8_t *)&startCode, 4, m_sendTimeout))<0)
        return res;

    *(uint8_t *)m_buf = type;
    *(uint16_t *)(m_buf+2) = proc;
    *(uint32_t *)(m_buf+4) = m_len;
    if ((res=m_link->send(m_buf, m_headerLen, m_sendTimeout))<0)
        return res;
    crc = calcCrc(m_buf, m_headerLen);

    if (m_len>=CRP_MAX_HEADER_LEN)
        chunk = CRP_MAX_HEADER_LEN;
    else
        chunk = m_len;
    if (m_link->send(m_buf, chunk, m_sendTimeout)<0)
        return CRP_RES_ERROR_SEND_TIMEOUT;

    // send crc
    crc += calcCrc(m_buf, chunk);
    if (m_link->send((uint8_t *)&crc, 2, m_sendTimeout)<0)
        return CRP_RES_ERROR_SEND_TIMEOUT;

    if ((res=recvAck(&ack, m_headerTimeout))<0)
        return res;

    if (ack)
        m_offset = chunk;
    else
        return CRP_RES_ERROR_CRC;

    return CRP_RES_OK;
}

int Chirp::sendData()
{
    uint16_t crc;
    uint32_t chunk;
    uint8_t sequence;
    bool ack;
    int res;

    for (sequence=0; m_offset<m_len; )
    {
        if (m_len-m_offset>=m_blkSize)
            chunk = m_blkSize;
        else
            chunk = m_len-m_offset;
        // send data
        if (m_link->send(m_buf+m_offset, chunk, m_sendTimeout)<0)
            return CRP_RES_ERROR_SEND_TIMEOUT;
        // send sequence
        if (m_link->send((uint8_t *)&sequence, 1, m_sendTimeout)<0)
            return CRP_RES_ERROR_SEND_TIMEOUT;
        // send crc
        crc = calcCrc(m_buf+m_offset, chunk) + calcCrc((uint8_t *)&sequence, 1);
        if (m_link->send((uint8_t *)&crc, 2, m_sendTimeout)<0)
            return CRP_RES_ERROR_SEND_TIMEOUT;

        if ((res=recvAck(&ack, m_dataTimeout))<0)
            return res;
        if (ack)
        {
            m_offset += chunk;
            sequence++;
        }
    }
    return CRP_RES_OK;
}

int Chirp::sendAck(bool ack) // false=nack
{
    uint8_t c;

    if (ack)
        c = CRP_ACK;
    else
        c = CRP_NACK;

    if (m_link->send(&c, 1, m_sendTimeout)<0)
        return CRP_RES_ERROR_SEND_TIMEOUT;

    return CRP_RES_OK;
}

int Chirp::recvHeader(uint8_t *type, ChirpProc *proc, bool wait)
{
    int res;
    uint8_t c;
    uint32_t chunk, startCode = 0;
    uint16_t crc, rcrc;

    if ((res=m_link->receive(&c, 1, wait?m_headerTimeout:0))<0)
        return res;
    if (res<1)
        return CRP_RES_ERROR;

    // find start code
    while(1)
    {
        startCode >>= 8;
        startCode |= (uint32_t)c<<24;
        if (startCode==CRP_START_CODE)
            break;
        if ((res=m_link->receive(&c, 1, m_idleTimeout))<0)
            return res;
        if (res<1)
            return CRP_RES_ERROR;
    }
    // receive rest of header
    if (m_link->receive(m_buf, m_headerLen, m_idleTimeout)<0)
        return CRP_RES_ERROR_RECV_TIMEOUT;
    if (res<(int)m_headerLen)
        return CRP_RES_ERROR;
    *type = *(uint8_t *)m_buf;
    *proc = *(ChirpProc *)(m_buf+2);
    m_len = *(uint32_t *)(m_buf+4);
    crc = calcCrc(m_buf, m_headerLen);

    if (m_len>=CRP_MAX_HEADER_LEN-m_headerLen)
        chunk = CRP_MAX_HEADER_LEN-m_headerLen;
    else
        chunk = m_len;
    if ((res=m_link->receive(m_buf, chunk+2, m_idleTimeout))<0) // +2 for crc
        return res;
    if (res<(int)chunk+2)
        return CRP_RES_ERROR;
    copyAlign((char *)&rcrc, (char *)(m_buf+chunk), 2);
    if (rcrc==crc+calcCrc(m_buf, chunk))
    {
        m_offset = chunk;
        sendAck(true);
    }
    else
    {
        sendAck(false); // send nack
        return CRP_RES_ERROR_CRC;
    }

    return CRP_RES_OK;
}

int Chirp::recvFull(uint8_t *type, ChirpProc *proc, bool wait)
{
    int res;
    uint32_t startCode;

    // receive header, with startcode check to make sure we're synced
    while(1)
    {
        if ((res=m_link->receive(m_buf, CRP_MAX_HEADER_LEN, wait?m_headerTimeout:0))<0)
            return res;
        // check to see if we received less data than expected
        if (res<CRP_MAX_HEADER_LEN)
            return CRP_RES_ERROR;

        startCode = *(uint32_t *)m_buf;
        if (startCode==CRP_START_CODE)
            break;
    }
    *type = *(uint8_t *)(m_buf+4);
    *proc = *(ChirpProc *)(m_buf+6);
    m_len = *(uint32_t *)(m_buf+8);

    if (m_len+m_headerLen>m_bufSize && (res=realloc(m_len+m_headerLen))<0)
        return res;

    if (m_len+m_headerLen>CRP_MAX_HEADER_LEN && !m_sharedMem)
    {
        if ((res=m_link->receive(m_buf+CRP_MAX_HEADER_LEN, m_len-(CRP_MAX_HEADER_LEN-m_headerLen), m_idleTimeout))<0)
            return res;
        // check to see if we received less data than expected
        if (res<(int)m_len-(CRP_MAX_HEADER_LEN-(int)m_headerLen))
            return CRP_RES_ERROR;
    }

    return CRP_RES_OK;
}

// We assume that the probability that we send a nack and the receiver interprets a nack is 100%
// We can't assume that the probability that we send an ack and the receiver interprets it is 100%
// Scenario
// 1) we receive packet 0, redo is 0, crc is good, we increment offset, send ack. (inc) (inc) rs=0, s=0, inc
// 2) we receive packet 1, crc is bad, we send nack (!inc) (!inc) rs=1, s=1
// 3) sender gets nack, so it resends
// 4) we receive packet 1, redo is 1, crc is good, we don't increment offset, send ack (inc) (inc) rs=1, s=1
// 5) we receive packet 2, redo is 0, crc is bad, we send nack (!inc) (!inc) rs=2, s=2
// 6) we receive packet 2, redo is 1, crc is good, we send ack (inc) (inc) rs=2, s=2
// 7) we receive packet 3, redo is 0, crc is good, we send ack (inc) (inc)
// different scenario
// 1) we receive packet 0, redo is 0, crc is good, we increment offset, send ack. (inc) (inc) rs=0, s=0
// 2) sender thinks it gets a nack, so it resends
// 3) we receive packet 0 again, but crc is bad, we send nack (!inc) (!inc) rs=1, s=0
// 4) sender gets nack, so it resends
// 5) we receive packet 0, redo is 1, crc is good, we don't increment offset, send ack (!inc) (inc) rs=1, s=0
// (we've essentially thrown out this packet, but that's ok, because we have a good packet 0)
// 6) we receive packet 1, redo is 0, crc is bad, we send nack (!inc) (!inc) rs=1, s=1
// 7) we receive packet 1, redo is 1, crc is good, we send ack (inc) (inc) rs=1, s=1
// 8) we receive packet 2, redo is 0, crc is good, we send ack (inc) (inc) rs=2, s=2
// a redo flag is not sufficient to communicate which packet we're on because the sender can misinterpret
// any number of nacks
int Chirp::recvData()
{
    int res;
    uint32_t chunk;
    uint16_t crc;
    uint8_t sequence, rsequence, naks;

    if (m_len+3+m_headerLen>m_bufSize && (res=realloc(m_len+3+m_headerLen))<0) // +3 to read sequence, crc
        return res;

    for (rsequence=0, naks=0; m_offset<m_len; )
    {
        if (m_len-m_offset>=m_blkSize)
            chunk = m_blkSize;
        else
            chunk = m_len-m_offset;
        if (m_link->receive(m_buf+m_offset, chunk+3, m_dataTimeout)<0) // +3 to read sequence, crc
            return CRP_RES_ERROR_RECV_TIMEOUT;
        if (res<(int)chunk+3)
            return CRP_RES_ERROR;
        sequence = *(uint8_t *)(m_buf+m_offset+chunk);
        copyAlign((char *)&crc, (char *)(m_buf+m_offset+chunk+1), 2);
        if (crc==calcCrc(m_buf+m_offset, chunk+1))
        {
            if (rsequence==sequence)
            {
                m_offset += chunk;
                rsequence++;
            }
            sendAck(true);
            naks = 0;
        }
        else
        {
            sendAck(false);
            naks++;
            if (naks<m_maxNak)
                naks++;
            else
                return CRP_RES_ERROR_MAX_NAK;
        }
    }
    return CRP_RES_OK;
}

int Chirp::recvAck(bool *ack, uint16_t timeout) // false=nack
{
    int res;
    uint8_t c;
    if ((res=m_link->receive(&c, 1, timeout))<0)
        return CRP_RES_ERROR_RECV_TIMEOUT;
    if (res<1)
        return CRP_RES_ERROR;

    if (c==CRP_ACK)
        *ack = true;
    else
        *ack = false;

    return CRP_RES_OK;
}
