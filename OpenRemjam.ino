#include <Audio.h>
#include <EEPROM.h>
#include <Entropy.h>
#include <fnet.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <CmdParser.hpp>
#include <CmdBuffer.hpp>
#include <CmdCallback.hpp>
#include "NetworkJitterBufferPlayQueue.h"
#include "QueueController.h"

// Command line helpers:
CmdParser myParser;
CmdBuffer<64> myBuffer;
CmdCallback<4> myCallback;

EthernetUDP Udp;

uint8_t recv_buf[2*OPENREMJAM_MONO_PACKET_SIZE];
uint8_t send_buf[OPENREMJAM_MONO_PACKET_SIZE]; 

// set up audio input:
AudioControlSGTL5000 shield;
AudioInputI2S i2s_in;
// downmix stereo line input to mono and feed it into rec_queue:
AudioMixer4 input_mixer;
AudioRecordQueue rec_queue;
AudioConnection input_to_mixer_0(i2s_in, 0, input_mixer, 0);
AudioConnection input_to_mixer_1(i2s_in, 1, input_mixer, 1);
AudioConnection mixer_to_rec_queue(input_mixer, rec_queue);

// MAC address:
byte mac[6];

// qc cares for audio output:
QueueController qc;

// subindex is the position of an audio block within a network block
int subindex = 0;

// seqno is the sequence number of a network block. The receiver uses it for detecting packet loss and reordering.
uint32_t seqno = 0;

void functConnect(CmdParser *myParser) {
  String idString(myParser->getCmdParam(1));
  String ipString(myParser->getCmdParam(2));
  String portString(myParser->getCmdParam(3));

  int id = idString.toInt();
  IPAddress ip;
  int port = portString.toInt();
  
  if (id < 0 || id > 15 || !ip.fromString(ipString) || port > 65535 || port < 1) {
    Serial.println("Syntax: connect <id> <ip> <port>");
    Serial.println("<id> must be in range 0...15, <ip> must be a valid IPv4 address, port must be in range 1...65535");
    Serial.println("Example: connect 0 192.168.178.20 9000");
  } else {
    qc.connect(id, ip, port);
    Serial.printf("Queue %d: connected to %2d.%2d.%2d.%2d:%d\r\n", id, ip[0], ip[1], ip[2], ip[3], port);
  }
}

void functDisconnect(CmdParser *myParser) {
  String idString(myParser->getCmdParam(1));
  int id = idString.toInt();
  if (id < 0 || id > 15) {
    Serial.println("Syntax: disconnect <id>");
    Serial.println("<id> must be in range 0...15");
    Serial.println("Example: disconnect 0");
  } else {
    qc.disconnect(id);
    Serial.printf("Queue %d: disconnected\r\n", id);
  }
}

void functShow(CmdParser *myParser) {
  for (int i=0; i<16; ++i) {
    qc.printInfo(i);
  }
}

void enet_getmac(uint8_t *mac) {
  uint32_t m1 = HW_OCOTP_MAC1;
  uint32_t m2 = HW_OCOTP_MAC0;
  mac[0] = m1 >> 8;
  mac[1] = m1 >> 0;
  mac[2] = m2 >> 24;
  mac[3] = m2 >> 16;
  mac[4] = m2 >> 8;
  mac[5] = m2 >> 0;
}

void setup() {
  Entropy.Initialize();
  pinMode(13, OUTPUT); // LED output;
  Serial.begin(9600); // parameter value doesn't matter -- Teensy always uses USB full speed

  myCallback.addCmd("CONNECT", &functConnect);
  myCallback.addCmd("DISCONNECT", &functDisconnect);
  myCallback.addCmd("SHOW", &functShow);
  
  AudioMemory(16 * OPENREMJAM_AUDIO_BLOCKS_PER_NETWORK_BLOCK + 10);    // each of the 16 queues needs up to OPENREMJAM_AUDIO_BLOCKS_PER_NETWORK_BLOCK audio blocks, plus 10 blocks headroom (e.g. for audio input)
  shield.enable();

  Serial.println("OpenRemjam – ultra-low latency audio streaming solution for Teensy 4.1");
  
  // print configuration information:
  Serial.printf("Audio block size: %d samples\r\n", AUDIO_BLOCK_SAMPLES);
  Serial.printf("Number of audio blocks per datatgram: %d\r\n", OPENREMJAM_AUDIO_BLOCKS_PER_NETWORK_BLOCK);
  Serial.printf("One network packet corresponds to %d microseconds.\r\n", OPENREMJAM_MONO_PACKET_DURATION_US);
  if (OPENREMJAM_MONO_PACKET_DURATION_US > 10000) {
    Serial.println("*** WARNING: Teensy might run out of memory. Consider lower values for AUDIO_BLOCK_SAMPLES or OPENREMJAM_AUDIO_BLOCKS_PER_NETWORK_BLOCK ***");
  }

  enet_getmac(mac);
  Serial.printf("MAC = %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println("Initializing Ethernet with DHCP:");

  if (Ethernet.begin(mac) == 0) {
    Serial.println("Failed to configure Ethernet using DHCP");
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      Serial.println("Ethernet shield was not found.  Sorry, can't run without hardware. :(");
    } else if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println("Ethernet cable is not connected.");
    }
    // no point in carrying on, so do nothing forevermore:
    while (true) {
      Serial.println("Please reboot.");
      while(1);
    }
  }

  Udp.begin(OPENREMJAM_DEFAULT_UDP_PORT);

  Serial.print("Ethernet adapter is ready! Local IP address: ");
  Serial.println(Ethernet.localIP());

  /********************** Set output volume *******************/
  shield.volume(0.3);

  /********************** Setup queues: ***********************/
  // Loopback (makes you hear your local audio signal)
  qc.getQueue(0)->setIP(IPAddress(127,0,0,1));
  qc.getQueue(0)->setPort(OPENREMJAM_DEFAULT_UDP_PORT);

  // Example for remote host:
  //qc.getQueue(1)->setIP(IPAddress(192,168,178,34));
  //qc.getQueue(1)->setPort(OPENREMJAM_DEFAULT_UDP_PORT);

  // start recording samples:
  rec_queue.begin();
}

void loop() {
    // Serial.println("Main loop");
    // process locally recorded samples
    if (rec_queue.available() > 0) {
        digitalWrite(13, HIGH);
        uint8_t *bufptr = (uint8_t *)rec_queue.readBuffer();
        memcpy(&send_buf[subindex*AUDIO_BLOCK_SAMPLES*2], bufptr, AUDIO_BLOCK_SAMPLES *2);
        rec_queue.freeBuffer();
        subindex++;
        
        if (subindex == OPENREMJAM_AUDIO_BLOCKS_PER_NETWORK_BLOCK) { // we have one a full packet now!
            // set seqno:
            memcpy( &send_buf[subindex*AUDIO_BLOCK_SAMPLES*2], &seqno, 4);
            seqno++;
            // we have a new block. Send it to all of our remote hosts:
            for (int i = 0; i < 16; ++i) {
                if (qc.getQueue(i)->getPort() != 0) {
                  Udp.beginPacket(qc.getQueue(i)->getIP(), qc.getQueue(i)->getPort());
                  Udp.write(send_buf, OPENREMJAM_MONO_PACKET_SIZE);
                  Udp.endPacket();
                }
            }
            subindex=0;
        }
    }
    digitalWrite(13, LOW);

    // receive incomming packets:
    if (Udp.parsePacket() == OPENREMJAM_MONO_PACKET_SIZE) {
        // we have received something that looks valid...

        // look up queue index
        int qi = qc.getQueueIndexByIP(Udp.remoteIP(), Udp.remotePort());

        if (qi < 0) {
            // we don't have a queue for this remote host, yet.
            qi = qc.getFreeAutoconnectQueueIndex(); // find a suitable queue!
            if (qi >= 0) {
                qc.getQueue(qi)->setIP(Udp.remoteIP());
                qc.getQueue(qi)->setPort(Udp.remotePort());
            } else {
                //Serial.println("No free autoconnect queues!");
            }
        }

        if (qi >= 0) {
            Udp.read(recv_buf,sizeof(recv_buf));
            qc.getQueue(qi)->enqueue(recv_buf);
        }
    }

    // process cmd line input
    myCallback.updateCmdProcessing(&myParser, &myBuffer, &Serial);

    // maintain IP configuration using DHCP
    Ethernet.maintain();
}
