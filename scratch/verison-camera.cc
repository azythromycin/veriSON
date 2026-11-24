// veriSON: measured OpenCV camera workload over a simulated 5G NR uplink.
// Compatible target: ns-3.48 + 5G-LENA v5.1.
// This replays frame sizes/times, NOT actual JPEG image contents.

#include "ns3/antenna-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/nr-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("VerisonCamera");

struct CameraFrame
{
    uint32_t id;
    double timeMs;    // Elapsed camera time, normalized to the second CSV row
    uint32_t bytes;   // JPEG frame size measured by OpenCV
};

static std::vector<CameraFrame>
ReadCameraTrace(const std::string& fileName)
{
    std::ifstream file(fileName);
    if (!file)
        throw std::runtime_error("Cannot open camera trace: " + fileName);

    std::string line;
    if (!std::getline(file, line))
        throw std::runtime_error("CSV file is empty");

    std::vector<std::string> headers;
    std::istringstream headerStream(line);
    std::string field;
    while (std::getline(headerStream, field, ','))
        headers.push_back(field);

    auto indexOf = [&](const std::string& name) -> size_t {
        auto found = std::find(headers.begin(), headers.end(), name);
        if (found == headers.end())
            throw std::runtime_error("Missing CSV column: " + name);
        return static_cast<size_t>(found - headers.begin());
    };
    const size_t idIndex = indexOf("frame_id");
    const size_t timeIndex = indexOf("timestamp_ms");
    const size_t bytesIndex = indexOf("encoded_bytes");
    const size_t maxIndex = std::max({idIndex, timeIndex, bytesIndex});

    std::vector<CameraFrame> frames;
    bool skippedStartupFrame = false;
    double originMs = 0.0;

    while (std::getline(file, line))
    {
        if (line.empty())
            continue;
        std::vector<std::string> values;
        std::istringstream csvLine(line);
        while (std::getline(csvLine, field, ','))
            values.push_back(field);
        if (values.size() <= maxIndex)
            throw std::runtime_error("Malformed camera trace row");

        CameraFrame f{static_cast<uint32_t>(std::stoul(values.at(idIndex))),
                      std::stod(values.at(timeIndex)),
                      static_cast<uint32_t>(std::stoul(values.at(bytesIndex)))};
        if (!skippedStartupFrame)
        {
            skippedStartupFrame = true; // Ignore frame 0, before YOLO warmed up.
            continue;
        }
        if (frames.empty())
            originMs = f.timeMs;
        f.timeMs -= originMs;
        if (f.timeMs < 0 || !std::isfinite(f.timeMs) || f.bytes == 0)
            throw std::runtime_error("Invalid camera trace timestamp or frame size");
        if (!frames.empty() && f.timeMs < frames.back().timeMs)
            throw std::runtime_error("Camera timestamps are not monotonic");
        frames.push_back(f);
    }
    if (frames.empty())
        throw std::runtime_error("No camera frames found after skipping startup frame");
    return frames;
}

// An ns-3 application installed on the UE. It owns a UDP socket and schedules
// frame-sized bursts at the exact relative times measured by OpenCV.
class CameraTraceSender : public Application
{
  public:
    void Configure(Ipv4Address destination,
                   uint16_t port,
                   std::vector<CameraFrame> trace,
                   uint32_t payloadBytes)
    {
        m_destination = destination;
        m_port = port;
        m_frames = std::move(trace);
        m_payloadBytes = payloadBytes;
    }

    uint64_t GetPayloadBytesSent() const { return m_payloadBytesSent; }
    uint64_t GetPacketsSent() const { return m_packetsSent; }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        if (m_socket->Connect(InetSocketAddress(m_destination, m_port)) != 0)
            NS_FATAL_ERROR("Camera sender: UDP socket connect failed");

        for (const auto& frame : m_frames)
        {
            Simulator::Schedule(MilliSeconds(frame.timeMs),
                                &CameraTraceSender::SendFrame,
                                this,
                                frame.bytes);
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

    void SendFrame(uint32_t frameBytes)
    {
        uint32_t left = frameBytes;
        while (left > 0)
        {
            const uint32_t chunk = std::min(left, m_payloadBytes);
            Ptr<Packet> packet = Create<Packet>(chunk); // Placeholder, NOT JPEG pixels.
            const int sent = m_socket->Send(packet);
            if (sent < 0 || static_cast<uint32_t>(sent) != chunk)
            {
                ++m_failedPackets;
                // Do not silently count failed sends as offered traffic.
            }
            else
            {
                ++m_packetsSent;
                m_payloadBytesSent += chunk;
            }
            left -= chunk;
        }
    }

    Ptr<Socket> m_socket;
    Ipv4Address m_destination;
    uint16_t m_port = 0;
    std::vector<CameraFrame> m_frames;
    uint32_t m_payloadBytes = 1200;
    uint64_t m_payloadBytesSent = 0;
    uint64_t m_packetsSent = 0;
    uint64_t m_failedPackets = 0;
};

int main(int argc, char* argv[])
{
    std::string traceFile = "data/camera_trace.csv";
    uint32_t packetPayloadBytes = 1200;
    double appStartMs = 500.0;
    double drainMs = 2000.0;
    double bandwidthHz = 50e6;
    double carrierHz = 28e9;
    uint16_t numerology = 2;
    uint16_t port = 1234;

    CommandLine cmd(__FILE__);
    cmd.AddValue("traceFile", "CSV produced by OpenCV", traceFile);
    cmd.AddValue("packetPayloadBytes", "UDP payload bytes per ns-3 packet", packetPayloadBytes);
    cmd.AddValue("appStartMs", "Start time for camera sender in simulated milliseconds", appStartMs);
    cmd.AddValue("drainMs", "Time to drain queued packets after final frame (ms)", drainMs);
    cmd.AddValue("bandwidthHz", "NR channel bandwidth in Hz", bandwidthHz);
    cmd.AddValue("carrierHz", "NR carrier frequency in Hz", carrierHz);
    cmd.AddValue("numerology", "NR numerology", numerology);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(packetPayloadBytes == 0 || packetPayloadBytes > 1400,
                    "Use a UDP payload in the range 1..1400 bytes");
    NS_ABORT_MSG_IF(appStartMs < 0 || drainMs < 0, "Times must be nonnegative");

    std::vector<CameraFrame> trace;
    try
    {
        trace = ReadCameraTrace(traceFile);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Trace error: " << e.what() << '\n';
        return 1;
    }

    uint64_t measuredBytes = 0;
    for (const auto& f : trace)
        measuredBytes += f.bytes;
    const double lastFrameMs = trace.back().timeMs;
    const Time stopTime = MilliSeconds(appStartMs + lastFrameMs + drainMs);

    std::cout << "veriSON camera trace: " << trace.size() << " frames, "
              << measuredBytes << " JPEG bytes, " << lastFrameMs
              << " ms replay duration\n";

    // One gNB and one UE. All radio and core entities are virtual ns-3 nodes.
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
    NrHelper::GetGnbPhy(gnbDev.Get(0), 0)->SetAttribute(
        "Numerology", UintegerValue(numerology));
    NrHelper::GetGnbPhy(gnbDev.Get(0), 0)->SetAttribute(
        "TxPower", DoubleValue(35.0));

    auto [remoteHost, remoteHostAddress] =
        epc->SetupRemoteHost("100Gb/s", 2500, Seconds(0.0));

    InternetStackHelper internet;
    internet.Install(grid.GetUserTerminals());
    epc->AssignUeIpv4Address(ueDev);
    nr->AttachToClosestGnb(ueDev, gnbDev);

    // Uplink IP traffic needs a route from the UE to the EPC/remote host.
    Ptr<Node> cameraUe = grid.GetUserTerminals().Get(0);
    Ipv4StaticRoutingHelper routingHelper;
    Ptr<Ipv4StaticRouting> ueRoute =
        routingHelper.GetStaticRouting(cameraUe->GetObject<Ipv4>());
    ueRoute->SetDefaultRoute(epc->GetUeDefaultGatewayAddress(), 1);

    // Minimal *virtual* receiver, not a separate physical monitoring laptop.
    PacketSinkHelper receiver("ns3::UdpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer receiverApps = receiver.Install(remoteHost);
    receiverApps.Start(Seconds(0));
    receiverApps.Stop(stopTime);

    Ptr<CameraTraceSender> sender = CreateObject<CameraTraceSender>();
    sender->Configure(remoteHostAddress, port, std::move(trace), packetPayloadBytes);
    cameraUe->AddApplication(sender);
    sender->SetStartTime(MilliSeconds(appStartMs));
    sender->SetStopTime(stopTime);

    FlowMonitorHelper flowHelper;
    NodeContainer endpoints;
    endpoints.Add(cameraUe);
    endpoints.Add(remoteHost);
    Ptr<FlowMonitor> monitor = flowHelper.Install(endpoints);

    Simulator::Stop(stopTime);
    Simulator::Run();
    monitor->CheckForLostPackets();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\nCamera application sent " << sender->GetPacketsSent()
              << " UDP packets, " << sender->GetPayloadBytesSent()
              << " payload bytes\n";

    const auto stats = monitor->GetFlowStats();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());
    for (const auto& [id, s] : stats)
    {
        auto flow = classifier->FindFlow(id);
        if (flow.protocol != 17 || flow.destinationPort != port)
            continue;
        const double durationS = std::max(0.001, lastFrameMs / 1000.0);
        std::cout << "\nUDP uplink " << flow.sourceAddress << ':' << flow.sourcePort
                  << " -> " << flow.destinationAddress << ':' << flow.destinationPort
                  << "\n  Tx packets: " << s.txPackets
                  << "\n  Rx packets: " << s.rxPackets
                  << "\n  Lost packets: " << s.lostPackets
                  << "\n  Tx IP bytes: " << s.txBytes
                  << "\n  Rx IP bytes: " << s.rxBytes
                  << "\n  Rx throughput: " << s.rxBytes * 8.0 / durationS / 1e6
                  << " Mbps (over camera trace duration)\n";
        if (s.rxPackets)
        {
            std::cout << "  Mean network delay: "
                      << s.delaySum.GetSeconds() * 1000.0 / static_cast<double>(s.rxPackets)
                      << " ms\n  Mean jitter: "
                      << s.jitterSum.GetSeconds() * 1000.0 / static_cast<double>(s.rxPackets)
                      << " ms\n";
        }
    }

    auto sink = DynamicCast<PacketSink>(receiverApps.Get(0));
    std::cout << "Receiver UDP payload bytes: " << sink->GetTotalRx() << '\n';
    Simulator::Destroy();
    return 0;
}
