/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2011-2015  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors
 *and contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the
 *terms of the GNU General Public License as published by the Free Software
 *Foundation, either version 3 of the License, or (at your option) any later
 *version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY
 *WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 *A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see
 *<http://www.gnu.org/licenses/>.
 **/

// ndn-simple.cpp

#include "ns3/core-module.h"
#include "ns3/ndnSIM-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

namespace ns3 {

int main( int argc, char *argv[] ) {
  // setting default parameters for PointToPoint links and channels
  // Config::SetDefault( "ns3::PointToPointNetDevice::DataRate",
  //                     StringValue( "1Mbps" ) );
  // Config::SetDefault( "ns3::PointToPointChannel::Delay",
  //                     StringValue( "10ms" ) );
  // Config::SetDefault( "ns3::DropTailQueue::MaxPackets", StringValue( "20" )
  // );

  // Read optional command-line parameters (e.g., enable visualizer with ./waf
  // --run=<> --visualize
  CommandLine cmd;
  cmd.Parse( argc, argv );

  AnnotatedTopologyReader topologyReader( "", 1 );
  topologyReader.SetFileName(
      "src/ndnSIM/examples/topologies/torus-grid-5.txt" );
  topologyReader.Read();

  string strategy_name       = "Normal";
  string zipf                = "0.7"; // 齐普夫参数
  string cache_size          = "100";  // 缓存大小
  string request_rate        = "10";  // 请求速率
  string average_update_time = "20";  // 更新时间
  string pit_store_size      = "80";  // PITCS表大小
  string update_factor =
      "1.0"; // 更新因子，0表示同一内容内容更新时间不变，1表示更新时间在内容到期后必变
  string experiment_time = "150";

  string rate_trace = "rate-trace-" + strategy_name + "-" + zipf + "-" +
                      cache_size + "-" + request_rate + "-" +
                      average_update_time + "-" + pit_store_size + "-" +
                      update_factor + "-" + experiment_time + ".txt";
  string cs_trace = "cs-trace-" + strategy_name + "-" + zipf + "-" +
                    cache_size + "-" + request_rate + "-" +
                    average_update_time + "-" + pit_store_size + "-" +
                    update_factor + "-" + experiment_time + ".txt";

  // Install NDN stack on all nodes
  ndn::StackHelper ndnHelper;
  // ndnHelper.SetDefaultRoutes( true );
  ndnHelper.SetOldContentStore( "ns3::ndn::cs::Lru", "MaxSize", cache_size );
  // ndnHelper.setCsSize(100);
  // ndnHelper.setPolicy("nfd::cs::lru")
  ndnHelper.InstallAll();

  // Choosing forwarding strategy
  ndn::StrategyChoiceHelper::InstallAll( "/prefix",
                                         "/localhost/nfd/strategy/best-route" );
  ndn::GlobalRoutingHelper ndnGlobalRoutingHelper;
  ndnGlobalRoutingHelper.InstallAll();
  // Installing applications
  Ptr<Node> producer = Names::Find<Node>( "12" );
  // NodeContainer
  NodeContainer consumerNodes;
  for ( int i = 0; i < 25; i++ ) {
    consumerNodes.Add( Names::Find<Node>( std::to_string( i ) ) );
  }

  // Consumer0
  ndn::AppHelper consumerHelper( "ns3::ndn::ConsumerZipfMandelbrot" );
  consumerHelper.SetAttribute( "NumberOfContents", StringValue( "5000" ) );
  consumerHelper.SetAttribute( "q", StringValue( "0" ) );
  consumerHelper.SetAttribute( "s", StringValue( zipf ) );

  // ndn::AppHelper consumerHelper( "ns3::ndn::ConsumerCbr" );
  // consumerHelper.SetAttribute("MaxSeq",IntegerValue(1000));
  
  consumerHelper.SetAttribute( "Frequency", StringValue( request_rate ) );
  // Consumer will request /prefix/0, /prefix/1, ...
  // consumerHelper0.SetPrefix( "/prefix/c0" );
  // consumerHelper0.Install( nodes.Get( 0 ) ); // first node

  // consumerHelper0.SetPrefix( "/prefix/c5" );
  // consumerHelper0.Install( nodes.Get( 5 ) );

  // consumerHelper0.SetPrefix( "/prefix/c6" );
  // consumerHelper0.Install( nodes.Get( 6 ) );
  consumerHelper.SetPrefix( "/prefix" );
  consumerHelper.Install( consumerNodes );
  // Consumer1
  // ndn::AppHelper consumerHelper1( "ns3::ndn::ConsumerZipfMandelbrot" );

  // // Consumer will request /prefix/0, /prefix/1, ...
  // consumerHelper1.SetPrefix( "/prefix/c1" );
  // consumerHelper1.SetAttribute( "Frequency",
  //                               StringValue( "100" ) ); // 10 interests a
  //                               second
  // consumerHelper0.SetAttribute( "NumberOfContents", StringValue( "1000" ) );
  // consumerHelper1.Install( nodes.Get( 1 ) ); // first node

  // Producer
  ndn::AppHelper producerHelper( "ns3::ndn::Producer" );
  // Producer will reply to all requests starting with /prefix
  producerHelper.SetPrefix( "/prefix" );
  producerHelper.SetAttribute( "PayloadSize", StringValue( "1024" ) );
  producerHelper.SetAttribute( "AverageUpdateTime",
                               StringValue( average_update_time ) );
  producerHelper.SetAttribute( "MaxPITStoreSize",
                               StringValue( pit_store_size ) );
  producerHelper.SetAttribute( "ExprimentTime",
                               StringValue( experiment_time ) );
  // producerHelper.Install( nodes.Get( 4 ) ); // last node
  producerHelper.Install( producer );
  ndnGlobalRoutingHelper.AddOrigins( "/prefix", producer );

  ndn::GlobalRoutingHelper::CalculateRoutes();

  Simulator::Stop( Seconds( stod( experiment_time ) ) );

  ndn::L3RateTracer::InstallAll( rate_trace, Seconds( 1.0 ) );

  // ndn::CsTracer::InstallAll( "cs-trace.txt", Seconds( 1 ) );

  Simulator::Run();
  Simulator::Destroy();

  return 0;
}

} // namespace ns3

int main( int argc, char *argv[] ) { return ns3::main( argc, argv ); }
