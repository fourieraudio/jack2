/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2008 Romain Moret at Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "JackNetInterface.h"
#include "JackException.h"

using namespace std;

namespace Jack
{
    // JackNetInterface*******************************************

    JackNetInterface::JackNetInterface() : fSocket()
    {
        fMulticastIP = NULL;
        fTxBuffer = NULL;
        fRxBuffer = NULL;
        fNetAudioCaptureBuffer = NULL;
        fNetAudioPlaybackBuffer = NULL;
        fNetMidiCaptureBuffer = NULL;
        fNetMidiPlaybackBuffer = NULL;
    }

    JackNetInterface::JackNetInterface ( const char* multicast_ip, int port ) : fSocket ( multicast_ip, port )
    {
        fMulticastIP = strdup ( multicast_ip );
        fTxBuffer = NULL;
        fRxBuffer = NULL;
        fNetAudioCaptureBuffer = NULL;
        fNetAudioPlaybackBuffer = NULL;
        fNetMidiCaptureBuffer = NULL;
        fNetMidiPlaybackBuffer = NULL;
    }

    JackNetInterface::JackNetInterface ( session_params_t& params, JackNetSocket& socket, const char* multicast_ip ) : fSocket ( socket )
    {
        fParams = params;
        fMulticastIP = strdup ( multicast_ip );
        fTxBuffer = NULL;
        fRxBuffer = NULL;
        fNetAudioCaptureBuffer = NULL;
        fNetAudioPlaybackBuffer = NULL;
        fNetMidiCaptureBuffer = NULL;
        fNetMidiPlaybackBuffer = NULL;
    }

    JackNetInterface::~JackNetInterface()
    {
        jack_log ( "JackNetInterface::~JackNetInterface" );

        fSocket.Close();
        delete[] fTxBuffer;
        delete[] fRxBuffer;
        delete[] fMulticastIP;
        delete fNetAudioCaptureBuffer;
        delete fNetAudioPlaybackBuffer;
        delete fNetMidiCaptureBuffer;
        delete fNetMidiPlaybackBuffer;
    }

    jack_nframes_t JackNetInterface::SetFramesPerPacket()
    {
        jack_log ( "JackNetInterface::SetFramesPerPacket" );

        if ( !fParams.fSendAudioChannels && !fParams.fReturnAudioChannels )
            return ( fParams.fFramesPerPacket = fParams.fPeriodSize );
        jack_nframes_t period = ( int ) powf ( 2.f, ( int ) ( log ( ( fParams.fMtu - sizeof ( packet_header_t ) )
                                               / ( max ( fParams.fReturnAudioChannels, fParams.fSendAudioChannels ) * sizeof ( sample_t ) ) ) / log ( 2 ) ) );
        return ( fParams.fFramesPerPacket = ( period > fParams.fPeriodSize ) ? fParams.fPeriodSize : period );
    }

    int JackNetInterface::SetNetBufferSize()
    {
        jack_log ( "JackNetInterface::SetNetBufferSize" );

        float audio_size, midi_size;
        int bufsize, res = 0;
        //audio
        audio_size = fParams.fMtu * ( fParams.fPeriodSize / fParams.fFramesPerPacket );
        //midi
        midi_size = fParams.fMtu * ( max ( fParams.fSendMidiChannels, fParams.fReturnMidiChannels ) *
                                     fParams.fPeriodSize * sizeof ( sample_t ) / ( fParams.fMtu - sizeof ( packet_header_t ) ) );
        //bufsize = sync + audio + midi
        bufsize = fParams.fMtu + ( int ) audio_size + ( int ) midi_size;

        //tx buffer
        if ( fSocket.SetOption ( SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof ( bufsize ) ) == SOCKET_ERROR )
            res = SOCKET_ERROR;

        //rx buffer
        if ( fSocket.SetOption ( SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof ( bufsize ) ) == SOCKET_ERROR )
            res = SOCKET_ERROR;

        return res;
    }

    int JackNetInterface::GetNMidiPckt()
    {
        //even if there is no midi data, jack need an empty buffer to know there is no event to read
        //99% of the cases : all data in one packet
        if ( fTxHeader.fMidiDataSize <= ( fParams.fMtu - sizeof ( packet_header_t ) ) )
            return 1;
        //else, get the number of needed packets (simply slice the biiig buffer)
        int npckt = fTxHeader.fMidiDataSize / ( fParams.fMtu - sizeof ( packet_header_t ) );
        if ( fTxHeader.fMidiDataSize % ( fParams.fMtu - sizeof ( packet_header_t ) ) )
            return ++npckt;
        return npckt;
    }

    bool JackNetInterface::IsNextPacket()
    {
        packet_header_t* rx_head = reinterpret_cast<packet_header_t*> ( fRxBuffer );
        //ignore first cycle
        if ( fRxHeader.fCycle <= 1 )
            return true;
        //same PcktID (cycle), next SubPcktID (subcycle)
        if ( ( fRxHeader.fSubCycle < ( fNSubProcess - 1 ) ) && ( rx_head->fCycle == fRxHeader.fCycle ) && ( rx_head->fSubCycle == ( fRxHeader.fSubCycle + 1 ) ) )
            return true;
        //next PcktID (cycle), SubPcktID reset to 0 (first subcyle)
        if ( ( rx_head->fCycle == ( fRxHeader.fCycle + 1 ) ) && ( fRxHeader.fSubCycle == ( fNSubProcess - 1 ) ) && ( rx_head->fSubCycle == 0 ) )
            return true;
        //else, packet(s) missing, return false
        return false;
    }

    void JackNetInterface::SetParams()
    {
        //number of audio subcycles (packets)
        fNSubProcess = fParams.fPeriodSize / fParams.fFramesPerPacket;

        //payload size
        fPayloadSize = fParams.fMtu - sizeof ( packet_header_t );

        //TX header init
        strcpy ( fTxHeader.fPacketType, "header" );
        fTxHeader.fID = fParams.fID;
        fTxHeader.fCycle = 0;
        fTxHeader.fSubCycle = 0;
        fTxHeader.fMidiDataSize = 0;
        fTxHeader.fBitdepth = fParams.fBitdepth;
        fTxHeader.fIsLastPckt = 0;

        //RX header init
        strcpy ( fRxHeader.fPacketType, "header" );
        fRxHeader.fID = fParams.fID;
        fRxHeader.fCycle = 0;
        fRxHeader.fSubCycle = 0;
        fRxHeader.fMidiDataSize = 0;
        fRxHeader.fBitdepth = fParams.fBitdepth;
        fRxHeader.fIsLastPckt = 0;

        //network buffers
        fTxBuffer = new char[fParams.fMtu];
        fRxBuffer = new char[fParams.fMtu];

        //net audio/midi buffers'addresses
        fTxData = fTxBuffer + sizeof ( packet_header_t );
        fRxData = fRxBuffer + sizeof ( packet_header_t );
    }

    // JackNetMasterInterface ************************************************************************************

    bool JackNetMasterInterface::Init()
    {
        jack_log ( "JackNetMasterInterface::Init, ID %u.", fParams.fID );

        session_params_t params;
        uint attempt = 0;
        int rx_bytes = 0;

        //socket
        if ( fSocket.NewSocket() == SOCKET_ERROR )
        {
            jack_error ( "Can't create socket : %s", StrError ( NET_ERROR_CODE ) );
            return false;
        }

        //timeout on receive (for init)
        if ( fSocket.SetTimeOut ( MASTER_INIT_TIMEOUT ) < 0 )
            jack_error ( "Can't set timeout : %s", StrError ( NET_ERROR_CODE ) );

        //connect
        if ( fSocket.Connect() == SOCKET_ERROR )
        {
            jack_error ( "Can't connect : %s", StrError ( NET_ERROR_CODE ) );
            return false;
        }

        //set the number of complete audio frames we can put in a packet
        SetFramesPerPacket();

        //send 'SLAVE_SETUP' until 'START_MASTER' received
        jack_info ( "Sending parameters to %s ...", fParams.fSlaveNetName );
        do
        {
            SetPacketType ( &fParams, SLAVE_SETUP );
            if ( fSocket.Send ( &fParams, sizeof ( session_params_t ), 0 ) == SOCKET_ERROR )
                jack_error ( "Error in send : ", StrError ( NET_ERROR_CODE ) );
            if ( ( ( rx_bytes = fSocket.Recv ( &params, sizeof ( session_params_t ), 0 ) ) == SOCKET_ERROR ) && ( fSocket.GetError() != NET_NO_DATA ) )
            {
                jack_error ( "Problem with network." );
                return false;
            }
        }
        while ( ( GetPacketType ( &params ) != START_MASTER ) && ( ++attempt < SLAVE_SETUP_RETRY ) );
        if ( attempt == SLAVE_SETUP_RETRY )
        {
            jack_error ( "Slave doesn't respond, exiting." );
            return false;
        }

        //set the new timeout for the socket
        if ( SetRxTimeout() == SOCKET_ERROR )
        {
            jack_error ( "Can't set rx timeout : %s", StrError ( NET_ERROR_CODE ) );
            return false;
        }

        //set the new rx buffer size
        if ( SetNetBufferSize() == SOCKET_ERROR )
        {
            jack_error ( "Can't set net buffer sizes : %s", StrError ( NET_ERROR_CODE ) );
            return false;
        }

        return true;
    }

    int JackNetMasterInterface::SetRxTimeout()
    {
        jack_log ( "JackNetMasterInterface::SetRxTimeout" );

        float time = 0;
        //slow or normal mode, short timeout on recv (2 audio subcycles)
        if ( ( fParams.fNetworkMode == 's' ) || ( fParams.fNetworkMode == 'n' ) )
            time = 2000000.f * ( static_cast<float> ( fParams.fFramesPerPacket ) / static_cast<float> ( fParams.fSampleRate ) );
        //fast mode, wait for 75% of the entire cycle duration
        else if ( fParams.fNetworkMode == 'f' )
            time = 750000.f * ( static_cast<float> ( fParams.fPeriodSize ) / static_cast<float> ( fParams.fSampleRate ) );
        return fSocket.SetTimeOut ( static_cast<int> ( time ) );
    }

    void JackNetMasterInterface::SetParams()
    {
        jack_log ( "JackNetMasterInterface::SetParams" );

        JackNetInterface::SetParams();

        fTxHeader.fDataStream = 's';
        fRxHeader.fDataStream = 'r';

        //midi net buffers
        fNetMidiCaptureBuffer = new NetMidiBuffer ( &fParams, fParams.fSendMidiChannels, fTxData );
        fNetMidiPlaybackBuffer = new NetMidiBuffer ( &fParams, fParams.fReturnMidiChannels, fRxData );

        //audio net buffers
        fNetAudioCaptureBuffer = new NetAudioBuffer ( &fParams, fParams.fSendAudioChannels, fTxData );
        fNetAudioPlaybackBuffer = new NetAudioBuffer ( &fParams, fParams.fReturnAudioChannels, fRxData );

        //audio netbuffer length
        fAudioTxLen = sizeof ( packet_header_t ) + fNetAudioPlaybackBuffer->GetSize();
        fAudioRxLen = sizeof ( packet_header_t ) + fNetAudioCaptureBuffer->GetSize();
    }

    void JackNetMasterInterface::Exit()
    {
        jack_log ( "JackNetMasterInterface::Exit, ID %u", fParams.fID );

        //stop process
        fRunning = false;
        //send a 'multicast euthanasia request' - new socket is required on macosx
        jack_info ( "Exiting '%s'", fParams.fName );
        SetPacketType ( &fParams, KILL_MASTER );
        JackNetSocket mcast_socket ( fMulticastIP, fSocket.GetPort() );
        if ( mcast_socket.NewSocket() == SOCKET_ERROR )
            jack_error ( "Can't create socket : %s", StrError ( NET_ERROR_CODE ) );
        if ( mcast_socket.SendTo ( &fParams, sizeof ( session_params_t ), 0, fMulticastIP ) == SOCKET_ERROR )
            jack_error ( "Can't send suicide request : %s", StrError ( NET_ERROR_CODE ) );
        mcast_socket.Close();
    }

    int JackNetMasterInterface::Send ( size_t size, int flags )
    {
        int tx_bytes;
        if ( ( ( tx_bytes = fSocket.Send ( fTxBuffer, size, flags ) ) == SOCKET_ERROR ) && fRunning )
        {
            net_error_t error = fSocket.GetError();
            if ( error == NET_CONN_ERROR )
            {
                //fatal connection issue, exit
                jack_error ( "'%s' : %s, exiting.", fParams.fName, StrError ( NET_ERROR_CODE ) );
                Exit();
            }
            else
                jack_error ( "Error in send : %s", StrError ( NET_ERROR_CODE ) );
        }
        return tx_bytes;
    }

    int JackNetMasterInterface::Recv ( size_t size, int flags )
    {
        int rx_bytes;
        if ( ( ( rx_bytes = fSocket.Recv ( fRxBuffer, size, flags ) ) == SOCKET_ERROR ) && fRunning )
        {
            net_error_t error = fSocket.GetError();
            //no data isn't really a network error, so just return 0 avalaible read bytes
            if ( error == NET_NO_DATA )
                return 0;
            else if ( error == NET_CONN_ERROR )
            {
                //fatal connection issue, exit
                jack_error ( "'%s' : %s, exiting.", fParams.fName, StrError ( NET_ERROR_CODE ) );
                //ask to the manager to properly remove the master
                Exit();
            }
            else
                jack_error ( "Error in receive : %s", StrError ( NET_ERROR_CODE ) );
        }
        return rx_bytes;
    }

    int JackNetMasterInterface::SyncSend()
    {
        fTxHeader.fCycle++;
        fTxHeader.fSubCycle = 0;
        fTxHeader.fDataType = 's';
        fTxHeader.fIsLastPckt = ( !fParams.fSendMidiChannels && !fParams.fSendAudioChannels ) ?  1 : 0;
        fTxHeader.fPacketSize = fParams.fMtu;
        memcpy ( fTxBuffer, &fTxHeader, sizeof ( packet_header_t ) );
        return Send ( fTxHeader.fPacketSize, 0 );
    }

    int JackNetMasterInterface::DataSend()
    {
        uint subproc;
        //midi
        if ( fParams.fSendMidiChannels )
        {
            //set global header fields and get the number of midi packets
            fTxHeader.fDataType = 'm';
            fTxHeader.fMidiDataSize = fNetMidiCaptureBuffer->RenderFromJackPorts();
            fTxHeader.fNMidiPckt = GetNMidiPckt();
            for ( subproc = 0; subproc < fTxHeader.fNMidiPckt; subproc++ )
            {
                fTxHeader.fSubCycle = subproc;
                fTxHeader.fIsLastPckt = ( ( subproc == ( fTxHeader.fNMidiPckt - 1 ) ) && !fParams.fSendAudioChannels ) ? 1 : 0;
                fTxHeader.fPacketSize = sizeof ( packet_header_t );
                fTxHeader.fPacketSize += fNetMidiCaptureBuffer->RenderToNetwork ( subproc, fTxHeader.fMidiDataSize );
                memcpy ( fTxBuffer, &fTxHeader, sizeof ( packet_header_t ) );
                if ( Send ( fTxHeader.fPacketSize, 0 ) == SOCKET_ERROR )
                    return SOCKET_ERROR;
            }
        }

        //audio
        if ( fParams.fSendAudioChannels )
        {
            fTxHeader.fDataType = 'a';
            for ( subproc = 0; subproc < fNSubProcess; subproc++ )
            {
                fTxHeader.fSubCycle = subproc;
                fTxHeader.fIsLastPckt = ( subproc == ( fNSubProcess - 1 ) ) ? 1 : 0;
                fTxHeader.fPacketSize = fAudioTxLen;
                memcpy ( fTxBuffer, &fTxHeader, sizeof ( packet_header_t ) );
                fNetAudioCaptureBuffer->RenderFromJackPorts ( subproc );
                if ( Send ( fTxHeader.fPacketSize, 0 ) == SOCKET_ERROR )
                    return SOCKET_ERROR;
            }
        }

        return 0;
    }

    int JackNetMasterInterface::SyncRecv()
    {
        int rx_bytes = 0;
        int cycle_offset = 0;
        packet_header_t* rx_head = reinterpret_cast<packet_header_t*> ( fRxBuffer );
        rx_bytes = Recv ( fParams.fMtu, MSG_PEEK );
        if ( ( rx_bytes == 0 ) || ( rx_bytes == SOCKET_ERROR ) )
            return rx_bytes;

        cycle_offset = fTxHeader.fCycle - rx_head->fCycle;

        switch ( fParams.fNetworkMode )
        {
            case 's' :
                //slow mode : allow to use full bandwidth and heavy process on the slave
                //  - extra latency is set to two cycles, one cycle for send/receive operations + one cycle for heavy process on the slave
                //  - if the network is two fast, just wait the next cycle, this mode allows a shorter cycle duration for the master
                //  - this mode will skip the two first cycles, thus it lets time for data to be processed and queued on the socket rx buffer
                //the slow mode is the safest mode because it wait twice the bandwidth relative time (send/return + process)
                if ( cycle_offset < 2 )
                    return 0;
                else
                    rx_bytes = Recv ( rx_head->fPacketSize, 0 );
                break;
                
            case 'n' :
                //normal use of the network :
                //  - extra latency is set to one cycle, what is the time needed to receive streams using full network bandwidth
                //  - if the network is too fast, just wait the next cycle, the benefit here is the master's cycle is shorter
                //  - indeed, data is supposed to be on the network rx buffer, so we don't have to wait for it
                if ( cycle_offset < 1 )
                    return 0;
                else
                    rx_bytes = Recv ( rx_head->fPacketSize, 0 );
                break;
                
            case 'f' :
                //fast mode suppose the network bandwith is larger than required for the transmission (only a few channels for example)
                //    - packets can be quickly received, quickly is here relative to the cycle duration
                //    - here, receive data, we can't keep it queued on the rx buffer,
                //    - but if there is a cycle offset, tell the user, that means we're not in fast mode anymore, network is too slow
                rx_bytes = Recv ( rx_head->fPacketSize, 0 );
                if ( cycle_offset )
                    jack_error ( "'%s' can't run in fast network mode, data received too late (%d cycle(s) offset)", fParams.fName, cycle_offset );
                break;
        }
        
        fRxHeader.fIsLastPckt = rx_head->fIsLastPckt;
        return rx_bytes;
    }

    int JackNetMasterInterface::DataRecv()
    {
        int rx_bytes = 0;
        uint jumpcnt = 0;
        uint midi_recvd_pckt = 0;
        packet_header_t* rx_head = reinterpret_cast<packet_header_t*> ( fRxBuffer );

        while ( !fRxHeader.fIsLastPckt )
        {
            //how much data is queued on the rx buffer ?
            rx_bytes = Recv ( fParams.fMtu, MSG_PEEK );
            if ( rx_bytes == SOCKET_ERROR )
                return rx_bytes;
            //if no data
            if ( ( rx_bytes == 0 ) && ( ++jumpcnt == fNSubProcess ) )
            {
                jack_error ( "No data from %s...", fParams.fName );
                jumpcnt = 0;
            }
            //else if data is valid,
            if ( rx_bytes && ( rx_head->fDataStream == 'r' ) && ( rx_head->fID == fParams.fID ) )
            {
                //read data
                switch ( rx_head->fDataType )
                {
                    case 'm':   //midi
                        Recv ( rx_head->fPacketSize, 0 );
                        fRxHeader.fCycle = rx_head->fCycle;
                        fRxHeader.fIsLastPckt = rx_head->fIsLastPckt;
                        fNetMidiPlaybackBuffer->RenderFromNetwork ( rx_head->fSubCycle, rx_bytes - sizeof ( packet_header_t ) );
                        if ( ++midi_recvd_pckt == rx_head->fNMidiPckt )
                            fNetMidiPlaybackBuffer->RenderToJackPorts();
                        jumpcnt = 0;
                        break;
                        
                    case 'a':   //audio
                        Recv ( rx_head->fPacketSize, 0 );
                        if ( !IsNextPacket() )
                            jack_error ( "Packet(s) missing from '%s'...", fParams.fName );
                        fRxHeader.fCycle = rx_head->fCycle;
                        fRxHeader.fSubCycle = rx_head->fSubCycle;
                        fRxHeader.fIsLastPckt = rx_head->fIsLastPckt;
                        fNetAudioPlaybackBuffer->RenderToJackPorts ( rx_head->fSubCycle );
                        jumpcnt = 0;
                        break;
                        
                    case 's':   //sync
                        if ( rx_head->fCycle == fTxHeader.fCycle )
                            return 0;
                }
            }
        }
        return rx_bytes;
    }

// JackNetSlaveInterface ************************************************************************************************

    uint JackNetSlaveInterface::fSlaveCounter = 0;

    bool JackNetSlaveInterface::Init()
    {
        jack_log ( "JackNetSlaveInterface::Init()" );

        //set the parameters to send
        strcpy ( fParams.fPacketType, "params" );
        fParams.fProtocolVersion = 'a';
        SetPacketType ( &fParams, SLAVE_AVAILABLE );

        //init loop : get a master and start, do it until connection is ok
        net_status_t status;
        do
        {
            //first, get a master, do it until a valid connection is running
            do
            {
                status = GetNetMaster();
                if ( status == NET_SOCKET_ERROR )
                    return false;
            }
            while ( status != NET_CONNECTED );

            //then tell the master we are ready
            jack_info ( "Initializing connection with %s...", fParams.fMasterNetName );
            status = SendStartToMaster();
            if ( status == NET_ERROR )
                return false;
        }
        while ( status != NET_ROLLING );

        return true;
    }

    net_status_t JackNetSlaveInterface::GetNetMaster()
    {
        jack_log ( "JackNetSlaveInterface::GetNetMaster()" );
        //utility
        session_params_t params;
        int rx_bytes = 0;

        //socket
        if ( fSocket.NewSocket() == SOCKET_ERROR )
        {
            jack_error ( "Fatal error : network unreachable - %s", StrError ( NET_ERROR_CODE ) );
            return NET_SOCKET_ERROR;
        }

        //bind the socket
        if ( fSocket.Bind() == SOCKET_ERROR )
            jack_error ( "Can't bind the socket : %s", StrError ( NET_ERROR_CODE ) );

        //timeout on receive
        if ( fSocket.SetTimeOut ( SLAVE_INIT_TIMEOUT ) == SOCKET_ERROR )
            jack_error ( "Can't set timeout : %s", StrError ( NET_ERROR_CODE ) );

        //disable local loop
        if ( fSocket.SetLocalLoop() == SOCKET_ERROR )
            jack_error ( "Can't disable multicast loop : %s", StrError ( NET_ERROR_CODE ) );

        //send 'AVAILABLE' until 'SLAVE_SETUP' received
        jack_info ( "Waiting for a master..." );
        do
        {
            //send 'available'
            if ( fSocket.SendTo ( &fParams, sizeof ( session_params_t ), 0, fMulticastIP ) == SOCKET_ERROR )
                jack_error ( "Error in data send : %s", StrError ( NET_ERROR_CODE ) );
            //filter incoming packets : don't exit while no error is detected
            rx_bytes = fSocket.CatchHost ( &params, sizeof ( session_params_t ), 0 );
            if ( ( rx_bytes == SOCKET_ERROR ) && ( fSocket.GetError() != NET_NO_DATA ) )
            {
                jack_error ( "Can't receive : %s", StrError ( NET_ERROR_CODE ) );
                return NET_RECV_ERROR;
            }
        }
        while ( strcmp ( params.fPacketType, fParams.fPacketType ) && ( GetPacketType ( &params ) != SLAVE_SETUP ) );

        //everything is OK, copy parameters
        fParams = params;

        //set the new buffer sizes
        if ( SetNetBufferSize() == SOCKET_ERROR )
            jack_error ( "Can't set net buffer sizes : %s", StrError ( NET_ERROR_CODE ) );

        //connect the socket
        if ( fSocket.Connect() == SOCKET_ERROR )
        {
            jack_error ( "Error in connect : %s", StrError ( NET_ERROR_CODE ) );
            return NET_CONNECT_ERROR;
        }

        return NET_CONNECTED;
    }

    net_status_t JackNetSlaveInterface::SendStartToMaster()
    {
        jack_log ( "JackNetSlaveInterface::SendStartToMaster" );

        //tell the master to start
        SetPacketType ( &fParams, START_MASTER );
        if ( fSocket.Send ( &fParams, sizeof ( session_params_t ), 0 ) == SOCKET_ERROR )
        {
            jack_error ( "Error in send : %s", StrError ( NET_ERROR_CODE ) );
            return ( fSocket.GetError() == NET_CONN_ERROR ) ? NET_ERROR : NET_SEND_ERROR;
        }
        return NET_ROLLING;
    }

    void JackNetSlaveInterface::SetParams()
    {
        jack_log ( "JackNetSlaveInterface::SetParams" );

        JackNetInterface::SetParams();

        fTxHeader.fDataStream = 'r';
        fRxHeader.fDataStream = 's';

        //midi net buffers
        fNetMidiCaptureBuffer = new NetMidiBuffer ( &fParams, fParams.fSendMidiChannels, fRxData );
        fNetMidiPlaybackBuffer = new NetMidiBuffer ( &fParams, fParams.fReturnMidiChannels, fTxData );

        //audio net buffers
        fNetAudioCaptureBuffer = new NetAudioBuffer ( &fParams, fParams.fSendAudioChannels, fRxData );
        fNetAudioPlaybackBuffer = new NetAudioBuffer ( &fParams, fParams.fReturnAudioChannels, fTxData );

        //audio netbuffer length
        fAudioTxLen = sizeof ( packet_header_t ) + fNetAudioPlaybackBuffer->GetSize();
        fAudioRxLen = sizeof ( packet_header_t ) + fNetAudioCaptureBuffer->GetSize();
    }

    int JackNetSlaveInterface::Recv ( size_t size, int flags )
    {
        int rx_bytes = fSocket.Recv ( fRxBuffer, size, flags );
        //handle errors
        if ( rx_bytes == SOCKET_ERROR )
        {
            net_error_t error = fSocket.GetError();
            //no data isn't really an error in realtime processing, so just return 0
            if ( error == NET_NO_DATA )
                jack_error ( "No data, is the master still running ?" );
            //if a network error occurs, this exception will restart the driver
            else if ( error == NET_CONN_ERROR )
            {
                jack_error ( "Connection lost." );
                throw JackNetException();
            }
            else
                jack_error ( "Fatal error in receive : %s", StrError ( NET_ERROR_CODE ) );
        }
        return rx_bytes;
    }

    int JackNetSlaveInterface::Send ( size_t size, int flags )
    {
        int tx_bytes = fSocket.Send ( fTxBuffer, size, flags );
        //handle errors
        if ( tx_bytes == SOCKET_ERROR )
        {
            net_error_t error = fSocket.GetError();
            //if a network error occurs, this exception will restart the driver
            if ( error == NET_CONN_ERROR )
            {
                jack_error ( "Connection lost." );
                throw JackNetException();
            }
            else
                jack_error ( "Fatal error in send : %s", StrError ( NET_ERROR_CODE ) );
        }
        return tx_bytes;
    }

    int JackNetSlaveInterface::SyncRecv()
    {
        int rx_bytes = 0;
        packet_header_t* rx_head = reinterpret_cast<packet_header_t*> ( fRxBuffer );
        //receive sync (launch the cycle)
        do
        {
            rx_bytes = Recv ( fParams.fMtu, 0 );
            //connection issue, send will detect it, so don't skip the cycle (return 0)
            if ( rx_bytes == SOCKET_ERROR )
                return rx_bytes;
        }
        while ( !rx_bytes && ( rx_head->fDataType != 's' ) );
        fRxHeader.fIsLastPckt = rx_head->fIsLastPckt;
        return rx_bytes;
    }

    int JackNetSlaveInterface::DataRecv()
    {
        uint recvd_midi_pckt = 0;
        int rx_bytes = 0;
        packet_header_t* rx_head = reinterpret_cast<packet_header_t*> ( fRxBuffer );

        while ( !fRxHeader.fIsLastPckt )
        {
            rx_bytes = Recv ( fParams.fMtu, MSG_PEEK );
            //error here, problem with recv, just skip the cycle (return -1)

            if ( rx_bytes == SOCKET_ERROR )
                return rx_bytes;
            if ( rx_bytes && ( rx_head->fDataStream == 's' ) && ( rx_head->fID == fParams.fID ) )
            {
                switch ( rx_head->fDataType )
                {
                    case 'm':   //midi
                        rx_bytes = Recv ( rx_head->fPacketSize, 0 );
                        fRxHeader.fCycle = rx_head->fCycle;
                        fRxHeader.fIsLastPckt = rx_head->fIsLastPckt;
                        fNetMidiCaptureBuffer->RenderFromNetwork ( rx_head->fSubCycle, rx_bytes - sizeof ( packet_header_t ) );
                        if ( ++recvd_midi_pckt == rx_head->fNMidiPckt )
                            fNetMidiCaptureBuffer->RenderToJackPorts();
                        break;
                        
                    case 'a':   //audio
                        rx_bytes = Recv ( rx_head->fPacketSize, 0 );
                        if ( !IsNextPacket() )
                            jack_error ( "Packet(s) missing..." );
                        fRxHeader.fCycle = rx_head->fCycle;
                        fRxHeader.fSubCycle = rx_head->fSubCycle;
                        fRxHeader.fIsLastPckt = rx_head->fIsLastPckt;
                        fNetAudioCaptureBuffer->RenderToJackPorts ( rx_head->fSubCycle );
                        break;
         
                    case 's':   //sync
                        jack_info ( "NetSlave : overloaded, skipping receive." );
                        return 0;
                }
            }
        }
        fRxHeader.fCycle = rx_head->fCycle;
        return 0;
    }

    int JackNetSlaveInterface::SyncSend()
    {
        //tx header
        if ( fParams.fSlaveSyncMode )
            fTxHeader.fCycle = fRxHeader.fCycle;
        else
            fTxHeader.fCycle++;
        fTxHeader.fSubCycle = 0;
        fTxHeader.fDataType = 's';
        fTxHeader.fIsLastPckt = ( !fParams.fReturnMidiChannels && !fParams.fReturnAudioChannels ) ?  1 : 0;
        fTxHeader.fPacketSize = fParams.fMtu;
        memcpy ( fTxBuffer, &fTxHeader, sizeof ( packet_header_t ) );
        return Send ( fTxHeader.fPacketSize, 0 );
    }

    int JackNetSlaveInterface::DataSend()
    {
        uint subproc;

        //midi
        if ( fParams.fReturnMidiChannels )
        {
            fTxHeader.fDataType = 'm';
            fTxHeader.fMidiDataSize = fNetMidiPlaybackBuffer->RenderFromJackPorts();
            fTxHeader.fNMidiPckt = GetNMidiPckt();
            for ( subproc = 0; subproc < fTxHeader.fNMidiPckt; subproc++ )
            {
                fTxHeader.fSubCycle = subproc;
                fTxHeader.fIsLastPckt = ( ( subproc == ( fTxHeader.fNMidiPckt - 1 ) ) && !fParams.fReturnAudioChannels ) ? 1 : 0;
                fTxHeader.fPacketSize = sizeof ( packet_header_t );
                fTxHeader.fPacketSize += fNetMidiPlaybackBuffer->RenderToNetwork ( subproc, fTxHeader.fMidiDataSize );
                memcpy ( fTxBuffer, &fTxHeader, sizeof ( packet_header_t ) );
                if ( Send ( fTxHeader.fPacketSize, 0 ) == SOCKET_ERROR )
                    return SOCKET_ERROR;
            }
        }

        //audio
        if ( fParams.fReturnAudioChannels )
        {
            fTxHeader.fDataType = 'a';
            for ( subproc = 0; subproc < fNSubProcess; subproc++ )
            {
                fTxHeader.fSubCycle = subproc;
                fTxHeader.fIsLastPckt = ( subproc == ( fNSubProcess - 1 ) ) ? 1 : 0;
                fTxHeader.fPacketSize = fAudioTxLen;
                memcpy ( fTxBuffer, &fTxHeader, sizeof ( packet_header_t ) );
                fNetAudioPlaybackBuffer->RenderFromJackPorts ( subproc );
                if ( Send ( fTxHeader.fPacketSize, 0 ) == SOCKET_ERROR )
                    return SOCKET_ERROR;
            }
        }
        return 0;
    }
}