// veriSON live webcam workload -> simulated NR uplink.
// Target: ns-3.48 + 5G-LENA v5.1, Linux/POSIX.
// This transmits simulated placeholder payloads matching live JPEG frame sizes,
// NOT the original JPEG pixels. Port 5005 is real localhost UDP; 1234 is
// a separate simulated UDP destination port inside ns-3.

#include "ns3/antenna-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/nr-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("VerisonLive");

// The sender is an ns-3 application. Only ns-3's simulation thread calls
// SendFrame(), StartApplication(), and StopApplication().
class LiveCameraSender : public Application
{
  public:
    void Configure(Ipv4Address destination, uint16_t port, uint32_t payloadBytes)
    {
        m_destination = destination;
        m_port = port;
        m_payloadBytes = payloadBytes;
    }

    void SendFrame(uint32_t frameId, uint32_t encodedBytes)
    {
        if (!m_socket)
        {
            ++m_framesSkipped;
            return;
        }
        ++m_framesProcessed;
        uint32_t remaining = encodedBytes;
        while (remaining > 0)
        {
            const uint32_t chunk = std::min(remaining, m_payloadBytes);
            Ptr<Packet> packet = Create<Packet>(chunk);
            const int result = m_socket->Send(packet);
            if (result == static_cast<int>(chunk))
            {
                ++m_packetsSent;
                m_payloadBytesSent += chunk;
            }
            else
            {
                ++m_packetsRejected;
            }
            remaining -= chunk;
        }
        // Show that new frames really are being consumed while ns-3 runs.
        if (m_framesProcessed % 30 == 0)
        {
            std::cout << "[sim " << Simulator::Now().GetSeconds()
                      << " s] processed frame " << frameId
                      << ", UDP payload sent " << m_payloadBytesSent << " bytes\n";
        }
    }

    uint64_t GetFramesProcessed() const { return m_framesProcessed; }
    uint64_t GetFramesSkipped() const { return m_framesSkipped; }
    uint64_t GetPacketsSent() const { return m_packetsSent; }
    uint64_t GetPacketsRejected() const { return m_packetsRejected; }
    uint64_t GetPayloadBytesSent() const { return m_payloadBytesSent; }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        if (m_socket->Connect(InetSocketAddress(m_destination, m_port)) != 0)
        {
            NS_FATAL_ERROR("Could not connect camera UE's simulated UDP socket");
        }
    }

    void StopApplication() override
    {
        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
    }

    Ptr<Socket> m_socket;
    Ipv4Address m_destination;
    uint16_t m_port = 0;
    uint32_t m_payloadBytes = 1200;
    uint64_t m_framesProcessed = 0;
    uint64_t m_framesSkipped = 0;
    uint64_t m_packetsSent = 0;
    uint64_t m_packetsRejected = 0;
    uint64_t m_payloadBytesSent = 0;
};

// Parse the compact message produced by yolo_camera.py:
//     frame_id,timestamp_ms,encoded_bytes
// Timestamp is recorded for provenance; live packet insertion is paced by
// message arrival (not by an already-elapsed absolute camera timestamp).
static bool
ParseTelemetry(const std::string& line, uint32_t& frameId, double& cameraMs, uint32_t& bytes)
{
    try
    {
        const size_t comma1 = line.find(',');
        if (comma1 == std::string::npos)
            return false;
        const size_t comma2 = line.find(',', comma1 + 1);
        if (comma2 == std::string::npos || line.find(',', comma2 + 1) != std::string::npos)
            return false;
        size_t usedId = 0, usedTime = 0, usedBytes = 0;
        const auto idStr = line.substr(0, comma1);
        const auto timeStr = line.substr(comma1 + 1, comma2 - comma1 - 1);
        const auto bytesStr = line.substr(comma2 + 1);
        const auto parsedId = std::stoul(idStr, &usedId);
        const double parsedTime = std::stod(timeStr, &usedTime);
        const auto parsedBytes = std::stoul(bytesStr, &usedBytes);
        if (usedId != idStr.size() || usedTime != timeStr.size() ||
            usedBytes != bytesStr.size() || !std::isfinite(parsedTime) || parsedTime < 0 ||
            parsedBytes == 0 || parsedBytes > 20'000'000 ||
            parsedId > UINT32_MAX || parsedBytes > UINT32_MAX)
            return false;
        frameId = static_cast<uint32_t>(parsedId);
        cameraMs = parsedTime;
        bytes = static_cast<uint32_t>(parsedBytes);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

static int
OpenTelemetrySocket(uint16_t listenPort)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        throw std::runtime_error(std::string("socket(): ") + std::strerror(errno));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(listenPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // only 127.0.0.1
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("Cannot bind localhost telemetry port " +
                                 std::to_string(listenPort) + ": " + reason +
                                 " (stop your temporary Python receiver)");
    }
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000; // wake every 200 ms to check stop flag
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
    {
        ::close(fd);
        throw std::runtime_error("Could not configure UDP socket timeout");
    }
    return fd;
}

// Runs on a REAL OS thread, never touches an ns-3 Socket directly.
// Simulator::ScheduleWithContext is the supported cross-thread handoff when
// RealtimeSimulatorImpl is selected. Network traffic is sent on sim thread.
static void
ListenForFrames(int fd,
                std::atomic<bool>& stop,
                std::atomic<uint64_t>& valid,
                std::atomic<uint64_t>& invalid,
                uint32_t ueContext,
                LiveCameraSender* sender)
{
    char buffer[512];
    while (!stop.load())
    {
        const ssize_t n = ::recvfrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            std::cerr << "Telemetry recvfrom error: " << std::strerror(errno) << '\n';
            break;
        }
        uint32_t frameId = 0, bytes = 0;
        double cameraMs = 0;
        if (!ParseTelemetry(std::string(buffer, static_cast<size_t>(n)),
                            frameId, cameraMs, bytes))
        {
            ++invalid;
            continue;
        }
        ++valid;
        // Important: do not call SendFrame() from this real OS thread.
        Simulator::ScheduleWithContext(ueContext,
                                       NanoSeconds(1),
                                       &LiveCameraSender::SendFrame,
                                       sender,
                                       frameId,
                                       bytes);
    }
}

static void
ReportProgress(Ptr<PacketSink> sink, Ptr<LiveCameraSender> sender,
               uint64_t& previousRxBytes)
{
    const uint64_t rx = sink->GetTotalRx();
    const double mbps = (rx - previousRxBytes) * 8.0 / 1e6;
    previousRxBytes = rx;
    std::cout << std::fixed << std::setprecision(2)
              << "[sim " << Simulator::Now().GetSeconds() << " s]"
              << " receiver: " << mbps << " Mbps (last 1 s),"
              << " frames fed: " << sender->GetFramesProcessed()
              << ", rejected UDP sends: " << sender->GetPacketsRejected() << '\n';
    Simulator::Schedule(Seconds(1), &ReportProgress, sink, sender,
                        std::ref(previousRxBytes));
}

int main(int argc, char* argv[])
{
    uint16_t listenPort = 5005;
    uint16_t simulatedPort = 1234;
    uint32_t payloadBytes = 1200;
    double runSeconds = 300.0;
    double bandwidthHz = 50e6;
    double carrierHz = 28e9;
    uint16_t numerology = 2;

    CommandLine cmd(__FILE__);
    cmd.AddValue("listenPort", "Real localhost UDP metadata port", listenPort);
    cmd.AddValue("runSeconds", "Wall-clock/simulation duration in seconds", runSeconds);
    cmd.AddValue("payloadBytes", "Maximum simulated UDP packet payload", payloadBytes);
    cmd.AddValue("bandwidthHz", "NR channel bandwidth in Hz", bandwidthHz);
    cmd.AddValue("carrierHz", "NR center frequency in Hz", carrierHz);
    cmd.AddValue("numerology", "NR numerology", numerology);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(runSeconds < 2 || !std::isfinite(runSeconds),
                    "runSeconds must be at least 2 seconds");
    NS_ABORT_MSG_IF(payloadBytes == 0 || payloadBytes > 1400,
                    "payloadBytes must be between 1 and 1400");

    // THIS MUST be configured before the first Simulator call.
    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::RealtimeSimulatorImpl"));

    // Reuse exactly the same NR network setup as the working CSV replay.
    GridScenarioHelper grid;
    grid.SetRows(1);
    grid.SetColumns(1);
    grid.SetHorizontalBsDistance(10.0);
    grid.SetVerticalBsDistance(10.0);
    grid.SetBsHeight(10.0);
    grid.SetUtHeight(1.5);
    grid.SetSectorization(GridScenarioHelper::SINGLE);
    grid.SetBsNumber(1);
    grid.SetUtNumber(1);
    grid.SetScenarioHeight(3);
    grid.SetScenarioLength(3);
    grid.CreateScenario();

    Ptr<NrPointToPointEpcHelper> epc = CreateObject<NrPointToPointEpcHelper>();
    Ptr<IdealBeamformingHelper> beamforming = CreateObject<IdealBeamformingHelper>();
    Ptr<NrHelper> nr = CreateObject<NrHelper>();
    nr->SetEpcHelper(epc);
    nr->SetBeamformingHelper(beamforming);

    CcBwpCreator bandCreator;
    CcBwpCreator::SimpleOperationBandConf bandConfig(carrierHz, bandwidthHz, 1);
    OperationBandInfo band = bandCreator.CreateOperationBandContiguousCc(bandConfig);
    Ptr<NrChannelHelper> channels = CreateObject<NrChannelHelper>();
    channels->ConfigureFactories("UMi", "Default", "ThreeGpp");
    channels->SetChannelConditionModelAttribute("UpdatePeriod", TimeValue(MilliSeconds(0)));
    channels->SetPathlossAttribute("ShadowingEnabled", BooleanValue(false));
    channels->AssignChannelsToBands({band});
    BandwidthPartInfoPtrVector bwps = CcBwpCreator::GetAllBwps({band});

    beamforming->SetAttribute("BeamformingMethod",
                              TypeIdValue(DirectPathBeamforming::GetTypeId()));
    epc->SetAttribute("S1uLinkDelay", TimeValue(MilliSeconds(0)));
    nr->SetUeAntennaAttribute("NumRows", UintegerValue(2));
    nr->SetUeAntennaAttribute("NumColumns", UintegerValue(4));
    nr->SetUeAntennaAttribute("AntennaElement",
                             PointerValue(CreateObject<IsotropicAntennaModel>()));
    nr->SetGnbAntennaAttribute("NumRows", UintegerValue(4));
    nr->SetGnbAntennaAttribute("NumColumns", UintegerValue(8));
    nr->SetGnbAntennaAttribute("AntennaElement",
                              PointerValue(CreateObject<IsotropicAntennaModel>()));
    nr->SetGnbBwpManagerAlgorithmAttribute("NGBR_LOW_LAT_EMBB", UintegerValue(0));
    nr->SetUeBwpManagerAlgorithmAttribute("NGBR_LOW_LAT_EMBB", UintegerValue(0));

    NetDeviceContainer gnbDev = nr->InstallGnbDevice(grid.GetBaseStations(), bwps);
    NetDeviceContainer ueDev = nr->InstallUeDevice(grid.GetUserTerminals(), bwps);
    NrHelper::GetGnbPhy(gnbDev.Get(0), 0)->SetAttribute("Numerology",
                                                       UintegerValue(numerology));
    NrHelper::GetGnbPhy(gnbDev.Get(0), 0)->SetAttribute("TxPower", DoubleValue(35.0));

    auto [remoteHost, remoteHostAddress] =
        epc->SetupRemoteHost("100Gb/s", 2500, Seconds(0.0));

    InternetStackHelper internet;
    internet.Install(grid.GetUserTerminals());
    epc->AssignUeIpv4Address(ueDev);
    nr->AttachToClosestGnb(ueDev, gnbDev);

    Ptr<Node> cameraUe = grid.GetUserTerminals().Get(0);
    Ipv4StaticRoutingHelper routingHelper;
    Ptr<Ipv4StaticRouting> ueRoute =
        routingHelper.GetStaticRouting(cameraUe->GetObject<Ipv4>());
    ueRoute->SetDefaultRoute(epc->GetUeDefaultGatewayAddress(), 1);

    const Time stopTime = Seconds(runSeconds);
    PacketSinkHelper receiver("ns3::UdpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), simulatedPort));
    ApplicationContainer receiverApps = receiver.Install(remoteHost);
    receiverApps.Start(Seconds(0));
    receiverApps.Stop(stopTime);
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(receiverApps.Get(0));

    Ptr<LiveCameraSender> sender = CreateObject<LiveCameraSender>();
    sender->Configure(remoteHostAddress, simulatedPort, payloadBytes);
    cameraUe->AddApplication(sender);
    sender->SetStartTime(Seconds(0));
    sender->SetStopTime(stopTime);

    FlowMonitorHelper flowHelper;
    NodeContainer endpoints;
    endpoints.Add(cameraUe);
    endpoints.Add(remoteHost);
    Ptr<FlowMonitor> monitor = flowHelper.Install(endpoints);

    // Bind before starting the real listener so port conflicts are clear.
    int fd;
    try
    {
        fd = OpenTelemetrySocket(listenPort);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Telemetry bridge error: " << e.what() << '\n';
        return 1;
    }

    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> accepted{0}, malformed{0};
    std::cout << "veriSON LIVE: listening on 127.0.0.1:" << listenPort
              << " (real UDP metadata)\n"
              << "5G-LENA uplink: simulated camera UE -> gNB -> remote host\n"
              << "Run your OpenCV camera now. Simulation duration: " << runSeconds
              << " seconds.\n" << std::flush;

    // The port binds before the simulation begins; socket reads run in parallel.
    std::thread listener(ListenForFrames, fd, std::ref(stopping),
                         std::ref(accepted), std::ref(malformed),
                         cameraUe->GetId(), PeekPointer(sender));

    uint64_t previousRxBytes = 0;
    Simulator::Schedule(Seconds(1), &ReportProgress, sink, sender,
                        std::ref(previousRxBytes));
    Simulator::Stop(stopTime);
    Simulator::Run();

    // Never let the listener inject additional ns-3 events after Run().
    stopping.store(true);
    listener.join(); // recvfrom() has a 200ms timeout
    ::close(fd);

    monitor->CheckForLostPackets();
    std::cout << "\n=== veriSON LIVE RESULTS ===\n"
              << "Metadata messages accepted: " << accepted.load() << '\n'
              << "Malformed metadata: " << malformed.load() << '\n'
              << "Camera frames processed: " << sender->GetFramesProcessed() << '\n'
              << "Camera frames skipped (sender not started): " << sender->GetFramesSkipped() << '\n'
              << "Simulated UDP sends: " << sender->GetPacketsSent() << '\n'
              << "Simulated UDP sends rejected: " << sender->GetPacketsRejected() << '\n'
              << "Sent simulated UDP payload bytes: " << sender->GetPayloadBytesSent() << '\n'
              << "Received simulated UDP payload bytes: " << sink->GetTotalRx() << '\n';

    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());
    for (const auto& [id, s] : monitor->GetFlowStats())
    {
        const auto tuple = classifier->FindFlow(id);
        if (tuple.protocol != 17 || tuple.destinationPort != simulatedPort)
            continue;
        std::cout << "\nUDP uplink " << tuple.sourceAddress << ':' << tuple.sourcePort
                  << " -> " << tuple.destinationAddress << ':' << tuple.destinationPort
                  << "\n  Tx packets: " << s.txPackets
                  << "\n  Rx packets: " << s.rxPackets
                  << "\n  FlowMonitor lost: " << s.lostPackets
                  << "\n  Tx IP bytes: " << s.txBytes
                  << "\n  Rx IP bytes: " << s.rxBytes << '\n';
        if (s.rxPackets > 0)
        {
            std::cout << "  Mean delay: " << (s.delaySum.GetSeconds() * 1000.0 / s.rxPackets)
                      << " ms\n  Mean jitter: " << (s.jitterSum.GetSeconds() * 1000.0 / s.rxPackets)
                      << " ms\n";
        }
    }
    Simulator::Destroy();
    return 0;
}
