/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014-2015,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne
 * University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the
 * terms
 * of the GNU General Public License as published by the Free Software
 * Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "forwarder.hpp"
#include "core/logger.hpp"
#include "core/random.hpp"
#include "face/null-face.hpp"
#include "strategy.hpp"

#include "utils/ndn-ns3-packet-tag.hpp"

#include <boost/random/uniform_int_distribution.hpp>
#include <random>

using namespace std;

namespace nfd {

NFD_LOG_INIT( "Forwarder" );

using fw::Strategy;

const Name Forwarder::LOCALHOST_NAME( "ndn:/localhost" );

Forwarder::Forwarder()
    : m_faceTable( *this )
    , m_fib( m_nameTree )
    , m_pit( m_nameTree )
    , m_measurements( m_nameTree )
    , m_strategyChoice( m_nameTree, fw::makeDefaultStrategy( *this ) )
    , m_csFace( make_shared<NullFace>( FaceUri( "contentstore://" ) ) ) {
  fw::installStrategies( *this );
  getFaceTable().addReserved( m_csFace, FACEID_CONTENT_STORE );
}

Forwarder::~Forwarder() {}

void Forwarder::onIncomingInterest( Face &inFace, const Interest &interest ) {
  int node = ns3::Simulator::GetContext();
  // receive Interest
  NFD_LOG_DEBUG( "onIncomingInterest face=" << inFace.getId()
                                            << " interest=" << interest.getName() );
  const_cast<Interest &>( interest ).setIncomingFaceId( inFace.getId() );
  ++m_counters.getNInInterests();

  // /localhost scope control
  bool isViolatingLocalhost = !inFace.isLocal() && LOCALHOST_NAME.isPrefixOf( interest.getName() );
  if ( isViolatingLocalhost ) {
    NFD_LOG_DEBUG( "onIncomingInterest face=" << inFace.getId() << " interest="
                                              << interest.getName() << " violates /localhost" );
    // (drop)
    return;
  }
  // PIT insert
  shared_ptr<pit::Entry> pitEntry = m_pit.insert( interest ).first;

  // detect duplicate Nonce
  int  dnw               = pitEntry->findNonce( interest.getNonce(), inFace );
  bool hasDuplicateNonce = ( dnw != pit::DUPLICATE_NONCE_NONE ) ||
                           m_deadNonceList.has( interest.getName(), interest.getNonce() );
  if ( hasDuplicateNonce ) {
    // goto Interest loop pipeline
    this->onInterestLoop( inFace, interest, pitEntry );
    return;
  }
  // cancel unsatisfy & straggler timer
  this->cancelUnsatisfyAndStragglerTimer( pitEntry );

  if ( interest.getInterestSignalFlag() == 1 ) {
    string PITListStr = interest.getInterestPITList();
    PITListStr += to_string( inFace.getId() ) + " ";
    const_cast<Interest &>( interest ).setInterestPITList( PITListStr );
    this->onInterestSignalForward( inFace, pitEntry, interest );
  } else {
    const pit::InRecordCollection &inRecords = pitEntry->getInRecords();
    bool                           isPending = inRecords.begin() != inRecords.end();
    if ( !isPending ) {
      shared_ptr<Data> match = m_csFromNdnSim->Lookup( interest.shared_from_this() );
      if ( match != nullptr ) {
        // 如果命中缓存，则需要向服务器查询是否为最新内容
        struct m_interest_entry ie;
        ie.m_name     = interest.getName();
        ie.m_interest = const_pointer_cast<Interest>( interest.shared_from_this() );
        ie.m_face     = const_pointer_cast<Face>( inFace.shared_from_this() );
        // ie.m_pitEntry = pitEntry;
        // ie.m_match = make_shared<Data>();
        // ie.m_match = match;
        // m_interest_store.push_back( ie );

        const_cast<Interest &>( interest ).setInterestSignalFlag( 1 );
        const_cast<Interest &>( interest ).setInterestTimestamp( match->getDataTimestamp() );
        const_cast<Interest &>( interest ).setInterestNodeIndex( node );
        this->onContentStoreHitCheck( inFace, pitEntry, interest );
      } else {
        this->onContentStoreMiss( inFace, pitEntry, interest );
      }
    } else {
      this->onContentStoreMiss( inFace, pitEntry, interest );
    }
  }
}

// bool Forwarder::onCheckInterestStore( const Interest &interest ) {}

void Forwarder::onContentStoreHitCheck( const Face &inFace, shared_ptr<pit::Entry> pitEntry,
                                        const Interest &interest ) {
  NFD_LOG_DEBUG( "onContentStoreHitCheck interest=" << interest.getName() );

  shared_ptr<Face> face = const_pointer_cast<Face>( inFace.shared_from_this() );
  // insert InRecord
  pitEntry->insertOrUpdateInRecord( face, interest );

  // set PIT unsatisfy timer
  this->setUnsatisfyTimer( pitEntry );

  // FIB lookup
  shared_ptr<fib::Entry> fibEntry = m_fib.findLongestPrefixMatch( *pitEntry );

  // dispatch to strategy
  this->dispatchToStrategy( pitEntry, bind( &Strategy::afterReceiveInterest, _1, cref( inFace ),
                                            cref( interest ), fibEntry, pitEntry ) );
  // m_pit.erase( pitEntry );
}

void Forwarder::onInterestSignalForward( const Face &inFace, shared_ptr<pit::Entry> pitEntry,
                                         const Interest &interest ) {
  NFD_LOG_DEBUG( "onInterestSignalForward interest=" << interest.getName() );

  shared_ptr<Face> face = const_pointer_cast<Face>( inFace.shared_from_this() );
  // insert InRecord
  pitEntry->insertOrUpdateInRecord( face, interest );

  // set PIT unsatisfy timer
  // this->setUnsatisfyTimer( pitEntry );

  // FIB lookup
  shared_ptr<fib::Entry> fibEntry = m_fib.findLongestPrefixMatch( *pitEntry );

  // dispatch to strategy
  this->dispatchToStrategy( pitEntry, bind( &Strategy::afterReceiveInterest, _1, cref( inFace ),
                                            cref( interest ), fibEntry, pitEntry ) );
  m_pit.erase( pitEntry );
}

void Forwarder::onContentStoreMiss( const Face &inFace, shared_ptr<pit::Entry> pitEntry,
                                    const Interest &interest ) {
  NFD_LOG_DEBUG( "onContentStoreMiss interest=" << interest.getName() );

  shared_ptr<Face> face = const_pointer_cast<Face>( inFace.shared_from_this() );
  // insert InRecord
  pitEntry->insertOrUpdateInRecord( face, interest );

  // set PIT unsatisfy timer
  this->setUnsatisfyTimer( pitEntry );

  // FIB lookup
  shared_ptr<fib::Entry> fibEntry = m_fib.findLongestPrefixMatch( *pitEntry );

  // dispatch to strategy
  this->dispatchToStrategy( pitEntry, bind( &Strategy::afterReceiveInterest, _1, cref( inFace ),
                                            cref( interest ), fibEntry, pitEntry ) );
}

void Forwarder::onContentStoreHit( const Face &inFace, shared_ptr<pit::Entry> pitEntry,
                                   const Interest &interest, const Data &data ) {
  NFD_LOG_DEBUG( "onContentStoreHit interest=" << interest.getName() );

  beforeSatisfyInterest( *pitEntry, *m_csFace, data );
  this->dispatchToStrategy( pitEntry, bind( &Strategy::beforeSatisfyInterest, _1, pitEntry,
                                            cref( *m_csFace ), cref( data ) ) );

  const_pointer_cast<Data>( data.shared_from_this() )->setIncomingFaceId( FACEID_CONTENT_STORE );
  // XXX should we lookup PIT for other Interests that also match csMatch?

  // set PIT straggler timer
  this->setStragglerTimer( pitEntry, true, data.getFreshnessPeriod() );

  // goto outgoing Data pipeline
  this->onOutgoingData( data, *const_pointer_cast<Face>( inFace.shared_from_this() ) );
}

void Forwarder::onInterestLoop( Face &inFace, const Interest &interest,
                                shared_ptr<pit::Entry> pitEntry ) {
  NFD_LOG_DEBUG( "onInterestLoop face=" << inFace.getId() << " interest=" << interest.getName() );

  // (drop)
}

/** \brief compare two InRecords for picking outgoing Interest
 *  \return true if b is preferred over a
 *
 *  This function should be passed to std::max_element over InRecordCollection.
 *  The outgoing Interest picked is the last incoming Interest
 *  that does not come from outFace.
 *  If all InRecords come from outFace, it's fine to pick that. This happens
 * when
 *  there's only one InRecord that comes from outFace. The legit use is for
 *  vehicular network; otherwise, strategy shouldn't send to the sole inFace.
 */
static inline bool compare_pickInterest( const pit::InRecord &a, const pit::InRecord &b,
                                         const Face *outFace ) {
  bool isOutFaceA = a.getFace().get() == outFace;
  bool isOutFaceB = b.getFace().get() == outFace;

  if ( !isOutFaceA && isOutFaceB ) {
    return false;
  }
  if ( isOutFaceA && !isOutFaceB ) {
    return true;
  }

  return a.getLastRenewed() > b.getLastRenewed();
}

void Forwarder::onOutgoingInterest( shared_ptr<pit::Entry> pitEntry, Face &outFace,
                                    bool wantNewNonce ) {
  if ( outFace.getId() == INVALID_FACEID ) {
    NFD_LOG_WARN( "onOutgoingInterest face=invalid interest=" << pitEntry->getName() );
    return;
  }
  NFD_LOG_DEBUG( "onOutgoingInterest face=" << outFace.getId()
                                            << " interest=" << pitEntry->getName() );

  // scope control
  if ( pitEntry->violatesScope( outFace ) ) {
    NFD_LOG_DEBUG( "onOutgoingInterest face=" << outFace.getId() << " interest="
                                              << pitEntry->getName() << " violates scope" );
    return;
  }

  // pick Interest
  const pit::InRecordCollection &         inRecords      = pitEntry->getInRecords();
  pit::InRecordCollection::const_iterator pickedInRecord = std::max_element(
      inRecords.begin(), inRecords.end(), bind( &compare_pickInterest, _1, _2, &outFace ) );
  BOOST_ASSERT( pickedInRecord != inRecords.end() );
  shared_ptr<Interest> interest =
      const_pointer_cast<Interest>( pickedInRecord->getInterest().shared_from_this() );

  if ( wantNewNonce ) {
    interest = make_shared<Interest>( *interest );
    static boost::random::uniform_int_distribution<uint32_t> dist;
    interest->setNonce( dist( getGlobalRng() ) );
  }

  // insert OutRecord
  pitEntry->insertOrUpdateOutRecord( outFace.shared_from_this(), *interest );

  // send Interest
  outFace.sendInterest( *interest );
  ++m_counters.getNOutInterests();
}

void Forwarder::onInterestReject( shared_ptr<pit::Entry> pitEntry ) {
  if ( pitEntry->hasUnexpiredOutRecords() ) {
    NFD_LOG_ERROR( "onInterestReject interest=" << pitEntry->getName()
                                                << " cannot reject forwarded Interest" );
    return;
  }
  NFD_LOG_DEBUG( "onInterestReject interest=" << pitEntry->getName() );

  // cancel unsatisfy & straggler timer
  this->cancelUnsatisfyAndStragglerTimer( pitEntry );

  // set PIT straggler timer
  this->setStragglerTimer( pitEntry, false );
}

void Forwarder::onInterestUnsatisfied( shared_ptr<pit::Entry> pitEntry ) {
  NFD_LOG_DEBUG( "onInterestUnsatisfied interest=" << pitEntry->getName() );

  // invoke PIT unsatisfied callback
  beforeExpirePendingInterest( *pitEntry );
  this->dispatchToStrategy( pitEntry,
                            bind( &Strategy::beforeExpirePendingInterest, _1, pitEntry ) );

  // goto Interest Finalize pipeline
  this->onInterestFinalize( pitEntry, false );
}

void Forwarder::onInterestFinalize( shared_ptr<pit::Entry> pitEntry, bool isSatisfied,
                                    const time::milliseconds &dataFreshnessPeriod ) {
  NFD_LOG_DEBUG( "onInterestFinalize interest="
                 << pitEntry->getName() << ( isSatisfied ? " satisfied" : " unsatisfied" ) );

  // Dead Nonce List insert if necessary
  this->insertDeadNonceList( *pitEntry, isSatisfied, dataFreshnessPeriod, 0 );

  // PIT delete
  this->cancelUnsatisfyAndStragglerTimer( pitEntry );
  m_pit.erase( pitEntry );
}

void Forwarder::onIncomingData( Face &inFace, const Data &data ) {
  int node = ns3::Simulator::GetContext();
  // receive Data
  NFD_LOG_DEBUG( "onIncomingData face=" << inFace.getId() << " data=" << data.getName() );
  const_cast<Data &>( data ).setIncomingFaceId( inFace.getId() );
  ++m_counters.getNInDatas();

  // /localhost scope control
  bool isViolatingLocalhost = !inFace.isLocal() && LOCALHOST_NAME.isPrefixOf( data.getName() );
  if ( isViolatingLocalhost ) {
    NFD_LOG_DEBUG( "onIncomingData face=" << inFace.getId() << " data=" << data.getName()
                                          << " violates /localhost" );
    // (drop)
    return;
  }

  if ( data.getDataSignalFlag() == 1 ) {
    NFD_LOG_DEBUG( "onIncomingDataSignal" );
    if ( data.getDataNodeIndex() == node ) {  // 到达信号发出节点
      NFD_LOG_DEBUG( "onIncomingDataSignal NodeIndex=node" );
      // cout<<node<<" : "<<data.getName()<<endl;
      const_cast<Data &>( data ).setDataSignalFlag( 0 );
      if ( data.getDataExpiration() == 1 ) { // 此数据为服务器刚响应的数据，按一般收到数据处理
        NFD_LOG_DEBUG( "onIncomingDataSignal Expiration=1" );
        // PIT match
        pit::DataMatchResult pitMatches = m_pit.findAllDataMatches( data );
        if ( pitMatches.begin() == pitMatches.end() ) {
          // goto Data unsolicited pipeline
          this->onDataUnsolicited( inFace, data );
          return;
        }

        shared_ptr<Data> dataCopyWithoutPacket = make_shared<Data>( data );
        dataCopyWithoutPacket->removeTag<ns3::ndn::Ns3PacketTag>();

        // CS insert
        m_csFromNdnSim->Erase( dataCopyWithoutPacket );
        m_csFromNdnSim->Add( dataCopyWithoutPacket );

        std::set<shared_ptr<Face>> pendingDownstreams;
        // foreach PitEntry
        for ( const shared_ptr<pit::Entry> &pitEntry : pitMatches ) {
          NFD_LOG_DEBUG( "onIncomingData matching=" << pitEntry->getName() );

          // cancel unsatisfy & straggler timer
          this->cancelUnsatisfyAndStragglerTimer( pitEntry );

          // remember pending downstreams
          const pit::InRecordCollection &inRecords = pitEntry->getInRecords();
          for ( pit::InRecordCollection::const_iterator it = inRecords.begin();
                it != inRecords.end(); ++it ) {
            if ( it->getExpiry() > time::steady_clock::now() ) {
              pendingDownstreams.insert( it->getFace() );
            }
          }

          // invoke PIT satisfy callback
          beforeSatisfyInterest( *pitEntry, inFace, data );
          this->dispatchToStrategy( pitEntry, bind( &Strategy::beforeSatisfyInterest, _1, pitEntry,
                                                    cref( inFace ), cref( data ) ) );

          // Dead Nonce List insert if necessary (for OutRecord of inFace)
          this->insertDeadNonceList( *pitEntry, true, data.getFreshnessPeriod(), &inFace );

          // mark PIT satisfied
          pitEntry->deleteInRecords();
          pitEntry->deleteOutRecord( inFace );

          // set PIT straggler timer
          this->setStragglerTimer( pitEntry, true, data.getFreshnessPeriod() );
        }

        // foreach pending downstream
        for ( std::set<shared_ptr<Face>>::iterator it = pendingDownstreams.begin();
              it != pendingDownstreams.end(); ++it ) {
          shared_ptr<Face> pendingDownstream = *it;
          if ( pendingDownstream.get() == &inFace ) {
            continue;
          }
          // goto outgoing Data pipeline
          this->onOutgoingData( data, *pendingDownstream );
        }
      } else { // 此数据包为通知未过期信号
        NFD_LOG_DEBUG( "onIncomingDataSignal Expiration=0" );
        NFD_LOG_DEBUG( "onContentStoreHit interest=" << data.getName() );
        // shared_ptr<Data> match =
        //     m_csFromNdnSim->Lookup( ( *( it->m_interest ) ).shared_from_this() );
        pit::DataMatchResult pitMatches = m_pit.findAllDataMatches( data );
        if ( pitMatches.begin() == pitMatches.end() ) {
          // goto Data unsolicited pipeline
          this->onDataUnsolicited( inFace, data );
          return;
        }
        std::set<shared_ptr<Face>> pendingDownstreams;
        // foreach PitEntry
        for ( const shared_ptr<pit::Entry> &pitEntry : pitMatches ) {
          NFD_LOG_DEBUG( "onIncomingData matching=" << pitEntry->getName() );

          // cancel unsatisfy & straggler timer
          this->cancelUnsatisfyAndStragglerTimer( pitEntry );

          // remember pending downstreams
          const pit::InRecordCollection &inRecords = pitEntry->getInRecords();
          for ( pit::InRecordCollection::const_iterator it = inRecords.begin();
                it != inRecords.end(); ++it ) {
            if ( it->getExpiry() > time::steady_clock::now() ) {
              pendingDownstreams.insert( it->getFace() );
            }
          }

          // invoke PIT satisfy callback
          beforeSatisfyInterest( *pitEntry, inFace, data );
          this->dispatchToStrategy( pitEntry, bind( &Strategy::beforeSatisfyInterest, _1, pitEntry,
                                                    cref( inFace ), cref( data ) ) );

          // Dead Nonce List insert if necessary (for OutRecord of inFace)
          this->insertDeadNonceList( *pitEntry, true, data.getFreshnessPeriod(), &inFace );

          // mark PIT satisfied
          pitEntry->deleteInRecords();
          pitEntry->deleteOutRecord( inFace );

          // set PIT straggler timer
          this->setStragglerTimer( pitEntry, true, data.getFreshnessPeriod() );
        }

        // foreach pending downstream
        for ( std::set<shared_ptr<Face>>::iterator it = pendingDownstreams.begin();
              it != pendingDownstreams.end(); ++it ) {
          shared_ptr<Face> pendingDownstream = *it;
          if ( pendingDownstream.get() == &inFace ) {
            continue;
          }
          // goto outgoing Data pipeline
          this->onOutgoingData( data, *pendingDownstream );
        }
      }
    } else {  // 信号回传路途中
      NFD_LOG_DEBUG( "onIncomingDataSignal NodeIndex!=node" );
      std::vector<std::string> res;
      std::string              PITList = data.getDataPITList();
      std::string              result;
      std::stringstream        input( PITList );
      while ( input >> result ) {
        res.push_back( result );
      }
      NFD_LOG_DEBUG( "onIncomingDataSignal NodeIndex!=node " << PITList );

      int         port            = std::stoi( res[ res.size() - 1 ] );
      std::string dataPITListTemp = "";
      for ( unsigned int i = 0; i < res.size() - 1; i++ ) {
        dataPITListTemp += res[ i ] + " ";
      }
      const_cast<Data &>( data ).setDataPITList( dataPITListTemp );
      shared_ptr<Face> outFace = Forwarder::getFace( port );
      // cout<<"22222222222222"<<port<<endl;
      if ( data.getDataExpiration() == 1 ) {
        NFD_LOG_DEBUG( "onIncomingDataSignal NodeIndex!=node Expiration=1" );
        const_cast<Data &>( data ).setDataSignalFlag( 0 );
        shared_ptr<Data> dataCopyWithoutPacket = make_shared<Data>( data );
        dataCopyWithoutPacket->removeTag<ns3::ndn::Ns3PacketTag>();
        // NFD_LOG_DEBUG( "onIncomingDataSignal NEW DATA TIMESTAMP "<<data.getDataTimestamp() );
        m_csFromNdnSim->Erase( dataCopyWithoutPacket );
        m_csFromNdnSim->Add( dataCopyWithoutPacket );
      }
      this->onOutgoingData( data, *outFace );
    }
  } else {
    NFD_LOG_DEBUG( "onIncomingNormalData" );
    // PIT match
    pit::DataMatchResult pitMatches = m_pit.findAllDataMatches( data );
    if ( pitMatches.begin() == pitMatches.end() ) {
      // goto Data unsolicited pipeline
      this->onDataUnsolicited( inFace, data );
      return;
    }

    shared_ptr<Data> dataCopyWithoutPacket = make_shared<Data>( data );
    dataCopyWithoutPacket->removeTag<ns3::ndn::Ns3PacketTag>();

    // CS insert
    if ( m_csFromNdnSim == nullptr )
      m_cs.insert( *dataCopyWithoutPacket );
    else {
      m_csFromNdnSim->Erase( dataCopyWithoutPacket );
      m_csFromNdnSim->Add( dataCopyWithoutPacket );
    }

    std::set<shared_ptr<Face>> pendingDownstreams;
    // foreach PitEntry
    for ( const shared_ptr<pit::Entry> &pitEntry : pitMatches ) {
      NFD_LOG_DEBUG( "onIncomingData matching=" << pitEntry->getName() );

      // cancel unsatisfy & straggler timer
      this->cancelUnsatisfyAndStragglerTimer( pitEntry );

      // remember pending downstreams
      const pit::InRecordCollection &inRecords = pitEntry->getInRecords();
      for ( pit::InRecordCollection::const_iterator it = inRecords.begin(); it != inRecords.end();
            ++it ) {
        if ( it->getExpiry() > time::steady_clock::now() ) {
          pendingDownstreams.insert( it->getFace() );
        }
      }

      // invoke PIT satisfy callback
      beforeSatisfyInterest( *pitEntry, inFace, data );
      this->dispatchToStrategy( pitEntry, bind( &Strategy::beforeSatisfyInterest, _1, pitEntry,
                                                cref( inFace ), cref( data ) ) );

      // Dead Nonce List insert if necessary (for OutRecord of inFace)
      this->insertDeadNonceList( *pitEntry, true, data.getFreshnessPeriod(), &inFace );

      // mark PIT satisfied
      pitEntry->deleteInRecords();
      pitEntry->deleteOutRecord( inFace );

      // set PIT straggler timer
      this->setStragglerTimer( pitEntry, true, data.getFreshnessPeriod() );
    }

    // foreach pending downstream
    for ( std::set<shared_ptr<Face>>::iterator it = pendingDownstreams.begin();
          it != pendingDownstreams.end(); ++it ) {
      shared_ptr<Face> pendingDownstream = *it;
      if ( pendingDownstream.get() == &inFace ) {
        continue;
      }
      // goto outgoing Data pipeline
      this->onOutgoingData( data, *pendingDownstream );
    }
  }
}

void Forwarder::onDataUnsolicited( Face &inFace, const Data &data ) {
  // accept to cache?
  bool acceptToCache = inFace.isLocal();
  if ( acceptToCache ) {
    // CS insert
    if ( m_csFromNdnSim == nullptr )
      m_cs.insert( data, true );
    else {
      m_csFromNdnSim->Erase( data.shared_from_this() );
      m_csFromNdnSim->Add( data.shared_from_this() );
    }
  }

  NFD_LOG_DEBUG( "onDataUnsolicited face=" << inFace.getId() << " data=" << data.getName()
                                           << ( acceptToCache ? " cached" : " not cached" ) );
}

void Forwarder::onOutgoingData( const Data &data, Face &outFace ) {
  if ( outFace.getId() == INVALID_FACEID ) {
    NFD_LOG_WARN( "onOutgoingData face=invalid data=" << data.getName() );
    return;
  }
  NFD_LOG_DEBUG( "onOutgoingData face=" << outFace.getId() << " data=" << data.getName() );

  // /localhost scope control
  bool isViolatingLocalhost = !outFace.isLocal() && LOCALHOST_NAME.isPrefixOf( data.getName() );
  if ( isViolatingLocalhost ) {
    NFD_LOG_DEBUG( "onOutgoingData face=" << outFace.getId() << " data=" << data.getName()
                                          << " violates /localhost" );
    // (drop)
    return;
  }

  // TODO traffic manager

  // send Data
  outFace.sendData( data );
  ++m_counters.getNOutDatas();
}

static inline bool compare_InRecord_expiry( const pit::InRecord &a, const pit::InRecord &b ) {
  return a.getExpiry() < b.getExpiry();
}

void Forwarder::setUnsatisfyTimer( shared_ptr<pit::Entry> pitEntry ) {
  const pit::InRecordCollection &         inRecords = pitEntry->getInRecords();
  pit::InRecordCollection::const_iterator lastExpiring =
      std::max_element( inRecords.begin(), inRecords.end(), &compare_InRecord_expiry );

  time::steady_clock::TimePoint lastExpiry        = lastExpiring->getExpiry();
  time::nanoseconds             lastExpiryFromNow = lastExpiry - time::steady_clock::now();
  if ( lastExpiryFromNow <= time::seconds( 0 ) ) {
    // TODO all InRecords are already expired; will this happen?
  }

  scheduler::cancel( pitEntry->m_unsatisfyTimer );
  pitEntry->m_unsatisfyTimer = scheduler::schedule(
      lastExpiryFromNow, bind( &Forwarder::onInterestUnsatisfied, this, pitEntry ) );
}

void Forwarder::setStragglerTimer( shared_ptr<pit::Entry> pitEntry, bool isSatisfied,
                                   const time::milliseconds &dataFreshnessPeriod ) {
  time::nanoseconds stragglerTime = time::milliseconds( 100 );

  scheduler::cancel( pitEntry->m_stragglerTimer );
  pitEntry->m_stragglerTimer =
      scheduler::schedule( stragglerTime, bind( &Forwarder::onInterestFinalize, this, pitEntry,
                                                isSatisfied, dataFreshnessPeriod ) );
}

void Forwarder::cancelUnsatisfyAndStragglerTimer( shared_ptr<pit::Entry> pitEntry ) {
  scheduler::cancel( pitEntry->m_unsatisfyTimer );
  scheduler::cancel( pitEntry->m_stragglerTimer );
}

static inline void insertNonceToDnl( DeadNonceList &dnl, const pit::Entry &pitEntry,
                                     const pit::OutRecord &outRecord ) {
  dnl.add( pitEntry.getName(), outRecord.getLastNonce() );
}

void Forwarder::insertDeadNonceList( pit::Entry &pitEntry, bool isSatisfied,
                                     const time::milliseconds &dataFreshnessPeriod,
                                     Face *                    upstream ) {
  // need Dead Nonce List insert?
  bool needDnl = false;
  if ( isSatisfied ) {
    bool hasFreshnessPeriod = dataFreshnessPeriod >= time::milliseconds::zero();
    // Data never becomes stale if it doesn't have FreshnessPeriod field
    needDnl = static_cast<bool>( pitEntry.getInterest().getMustBeFresh() ) &&
              ( hasFreshnessPeriod && dataFreshnessPeriod < m_deadNonceList.getLifetime() );
  } else {
    needDnl = true;
  }

  if ( !needDnl ) {
    return;
  }

  // Dead Nonce List insert
  if ( upstream == 0 ) {
    // insert all outgoing Nonces
    const pit::OutRecordCollection &outRecords = pitEntry.getOutRecords();
    std::for_each( outRecords.begin(), outRecords.end(),
                   bind( &insertNonceToDnl, ref( m_deadNonceList ), cref( pitEntry ), _1 ) );
  } else {
    // insert outgoing Nonce of a specific face
    pit::OutRecordCollection::const_iterator outRecord = pitEntry.getOutRecord( *upstream );
    if ( outRecord != pitEntry.getOutRecords().end() ) {
      m_deadNonceList.add( pitEntry.getName(), outRecord->getLastNonce() );
    }
  }
}

} // namespace nfd
