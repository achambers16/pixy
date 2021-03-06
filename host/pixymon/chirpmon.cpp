#include <QDebug>
#include <QMutexLocker>
#include "chirpmon.h"
#include "interpreter.h"

ChirpMon::ChirpMon(Interpreter *interpreter)
{
    m_hinterested = true;
    m_interpreter = interpreter;
}

ChirpMon::~ChirpMon()
{
}


int ChirpMon::serviceChirp()
{
    uint8_t type;
    ChirpProc recvProc;
    void *args[CRP_MAX_ARGS+1];
    int res;

    while(1)
    {
        if ((res=recvChirp(&type, &recvProc, args, true))<0)
            return res;
        handleChirp(type, recvProc, args);

        if (type&CRP_RESPONSE)
            break;
    }
    return 0;
}


int ChirpMon::open()
{
    int res;

    if ((res=m_link.open())<0)
        return res;

    setLink(&m_link);

    return 0;
}


int ChirpMon::init()
{
    return remoteInit();
}

int ChirpMon::handleChirp(uint8_t type, ChirpProc proc, void *args[])
{
    if (type==CRP_RESPONSE)
        return m_interpreter->handleResponse(args);

    return Chirp::handleChirp(type, proc, args);
}

int ChirpMon::sendChirp(uint8_t type, ChirpProc proc)
{   // this is only called when we call call()
    int res;

    // if we're programming (defining the program), put all the calls in m_program
    // otherwise pass the call the Chirp::sendChirp() so it gets sent out.
    // todo: save the call and use the chirp thread to send (so send and receive are handled by
    // same thread. not sure how important that is...)
    if (m_interpreter->m_programming && !(type&CRP_INTRINSIC))
    {
        // put on queue
        // only copy data (not header).  Header hasn't been written to buffer yet.
        m_interpreter->addProgram(ChirpCallData(type, proc, m_buf+m_headerLen, m_len));
        return 0;
    }

    res = Chirp::sendChirp(type, proc);

    return res;
}

int ChirpMon::execute(const ChirpCallData &data)
{
    int res;

    // copy into chirp buffer-- remember to skip the header space
    memcpy(m_buf+m_headerLen, data.m_buf, data.m_len);
    m_len = data.m_len;
    if ((res=Chirp::sendChirp(data.m_type, data.m_proc))<0)
        return res;
    if ((res=serviceChirp())<0)
        return res;

    return 0;
}

