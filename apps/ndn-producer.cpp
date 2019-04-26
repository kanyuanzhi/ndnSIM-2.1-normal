/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2011-2015  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors and
 * contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include "ndn-producer.hpp"
#include "ns3/log.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include "model/ndn-app-face.hpp"
#include "model/ndn-ns3.hpp"
#include "model/ndn-l3-protocol.hpp"
#include "helper/ndn-fib-helper.hpp"

#include <memory>
#include <random>

NS_LOG_COMPONENT_DEFINE("ndn.Producer");

using namespace std;

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED(Producer);

TypeId
Producer::GetTypeId(void)
{
  static TypeId tid =
    TypeId("ns3::ndn::Producer")
      .SetGroupName("Ndn")
      .SetParent<App>()
      .AddConstructor<Producer>()
      .AddAttribute("Prefix", "Prefix, for which producer has the data", StringValue("/"),
                    MakeNameAccessor(&Producer::m_prefix), MakeNameChecker())
      .AddAttribute(
         "Postfix",
         "Postfix that is added to the output data (e.g., for adding producer-uniqueness)",
         StringValue("/"), MakeNameAccessor(&Producer::m_postfix), MakeNameChecker())
      .AddAttribute("PayloadSize", "Virtual payload size for Content packets", UintegerValue(1024),
                    MakeUintegerAccessor(&Producer::m_virtualPayloadSize),
                    MakeUintegerChecker<uint32_t>())
      .AddAttribute("Freshness", "Freshness of data packets, if 0, then unlimited freshness",
                    TimeValue(Seconds(0)), MakeTimeAccessor(&Producer::m_freshness),
                    MakeTimeChecker())
      .AddAttribute(
         "Signature",
         "Fake signature, 0 valid signature (default), other values application-specific",
         UintegerValue(0), MakeUintegerAccessor(&Producer::m_signature),
         MakeUintegerChecker<uint32_t>())
      .AddAttribute("KeyLocator",
                    "Name to be used for key locator.  If root, then key locator is not used",
                    NameValue(), MakeNameAccessor(&Producer::m_keyLocator), MakeNameChecker())
      .AddAttribute(
         "AverageUpdateTime",
         "内容平均更新时间",
         UintegerValue(10), MakeUintegerAccessor(&Producer::m_averageUpdateTime),
         MakeUintegerChecker<uint32_t>());
  return tid;
}

Producer::Producer()
{
  NS_LOG_FUNCTION_NOARGS();
}

// inherited from Application base class.
void
Producer::StartApplication()
{
  NS_LOG_FUNCTION_NOARGS();
  App::StartApplication();

  FibHelper::AddRoute(GetNode(), m_prefix, m_face, 0);
}

void
Producer::StopApplication()
{
  NS_LOG_FUNCTION_NOARGS();

  App::StopApplication();
}

void
Producer::OnInterest(shared_ptr<const Interest> interest)
{
  double tnow = ns3::Simulator::Now().GetSeconds();
  int tnow_int = (int) tnow;
  App::OnInterest(interest); // tracing inside

  NS_LOG_FUNCTION(this << interest);

  if (!m_active)
    return;

  if ((int)(tnow * 10) % 10 != 0){
      updateFlag = false;
  }
  if ((int)(tnow * 10) % 10 == 0 && (int)tnow !=0){
    if (!updateFlag){
      cout<<"!!!!!!!!!!!!!!!!!!!!!!!!!!!!"<<tnow<<endl;
      updateFlag = true;
      list<contentTimestampEntry>::iterator it;
      for ( it=contentTimestampStore.begin(); it!=contentTimestampStore.end();++it ){
        if (tnow_int - it->lastUpdateTime >= it->updateTime){
          it->lastUpdateTime = tnow_int;
        }
      }
    }
  }
  cout<<"signalAccount: "<<signalAccount<<endl;
  cout<<"expirationSignalAccount: "<<expirationSignalAccount<<endl;


  if (interest->getInterestSignalFlag() == 1){
    signalAccount++;
    shared_ptr<Data> data = GenerateData(interest);
    data->setDataSignalFlag(1);
    data->setDataNodeIndex(interest->getInterestNodeIndex());
    if (CheckExpiration(interest)){
      data->setDataExpiration(1);
      expirationSignalAccount++;
    }else{
      data->setDataExpiration(0);
    }
    NS_LOG_INFO("node(" << GetNode()->GetId() << ") responding with Data: " << data->getName());

    // to create real wire encoding
    data->wireEncode();

    m_transmittedDatas(data, this, m_face);
    m_face->onReceiveData(*data);
    
  }else{
    random_device r;
    auto data = this->GenerateData(interest);

    list<contentTimestampEntry>::iterator it;
    bool exist = false;
    for ( it=contentTimestampStore.begin(); it!=contentTimestampStore.end();++it ){
      if (it->name == interest->getName()){
        exist = true;
        data->setDataTimestamp(it->lastUpdateTime);
        break;
      }
    }
    if (!exist){
      default_random_engine updateTime_e(r());
      uniform_int_distribution<int> updateTime_u(1, 2*m_averageUpdateTime-1);

      struct contentTimestampEntry cte;
      cte.name = interest->getName();
      cte.updateTime = updateTime_u(updateTime_e);

      default_random_engine lastUpdateTime_e(r());
      uniform_int_distribution<int> lastUpdateTime_u(tnow_int-cte.updateTime+1,tnow_int);
      cte.lastUpdateTime = lastUpdateTime_u(lastUpdateTime_e);

      contentTimestampStore.push_front(cte);
      data->setDataTimestamp(cte.lastUpdateTime);
    }
    // cout<<contentTimestampStore.size()<<endl;

    
  
    NS_LOG_INFO("node(" << GetNode()->GetId() << ") responding with Data: " << data->getName());

    // to create real wire encoding
    data->wireEncode();

    m_transmittedDatas(data, this, m_face);
    m_face->onReceiveData(*data);
  }

  
}

shared_ptr<Data> Producer::GenerateData(shared_ptr<const Interest> interest){
  Name dataName(interest->getName());
  auto data = make_shared<Data>();
  data->setName(dataName);
  data->setFreshnessPeriod(::ndn::time::milliseconds(m_freshness.GetMilliSeconds()));

  data->setContent(make_shared< ::ndn::Buffer>(m_virtualPayloadSize));

  Signature signature;
  SignatureInfo signatureInfo(static_cast< ::ndn::tlv::SignatureTypeValue>(255));

  if (m_keyLocator.size() > 0) {
    signatureInfo.setKeyLocator(m_keyLocator);
  }

  signature.setInfo(signatureInfo);
  signature.setValue(::ndn::nonNegativeIntegerBlock(::ndn::tlv::SignatureValue, m_signature));

  data->setSignature(signature);

  return data;
}


bool Producer::CheckExpiration(shared_ptr<const Interest> interest){
  list<contentTimestampEntry>::iterator it;
  for ( it=contentTimestampStore.begin(); it!=contentTimestampStore.end();++it ){
    if (it->name == interest->getName()){
      if (interest->getInterestTimestamp() == it->lastUpdateTime){
        // 兴趣包的时间戳与服务器中该内容的时间戳一致，说明没有过期
        return false;
      }else{
        return true;
      }
    }
  }
}

} // namespace ndn
} // namespace ns3
