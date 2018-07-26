////////////////////////////////////////////////////////////////////////
/// \file  ParticleListAction.cxx
/// \brief Use Geant4's user "hooks" to maintain a list of particles generated by Geant4.
///
/// \author  seligman@nevis.columbia.edu
////////////////////////////////////////////////////////////////////////

#include "larg4/pluginActions/ParticleListAction_service.h"
#include "nutools/G4Base/PrimaryParticleInformation.h"
#include "lardataobj/Simulation/sim.h"
#include "nutools/ParticleNavigation/ParticleList.h"

#include "messagefacility/MessageLogger/MessageLogger.h"

// Framework includes
#include "art/Framework/Core/ProducingService.h"
// framework includes:
#include "art/Framework/Services/Registry/ServiceMacros.h"
#include "canvas/Persistency/Common/Ptr.h"
#include "Geant4/G4Event.hh"
#include "Geant4/G4Track.hh"
#include "Geant4/G4ThreeVector.hh"
#include "Geant4/G4ParticleDefinition.hh"
#include "Geant4/G4PrimaryParticle.hh"
#include "Geant4/G4DynamicParticle.hh"
#include "Geant4/G4VUserPrimaryParticleInformation.hh"
#include "Geant4/G4Step.hh"
#include "Geant4/G4StepPoint.hh"
#include "Geant4/G4VProcess.hh"
#include "Geant4/G4String.hh"

#include <TLorentzVector.h>
#include <TString.h>


#include <algorithm>

// unused const G4bool debug = false;

// Photon variables defined at each step, for use 
// in temporary velocity bug fix. -wforeman                          
double globalTime, velocity_G4, velocity_step;
bool entra = true;

namespace larg4 {
  
  // Initialize static members.
  int ParticleListActionService::fCurrentTrackID = sim::NoParticleId;
  int ParticleListActionService::fTrackIDOffset = 0;
  
  //----------------------------------------------------------------------------
  // Dropped particle test
  
  bool ParticleListActionService::isDropped(simb::MCParticle const* p) {
    return !p || p->Trajectory().empty();
  } // ParticleListActionService::isDropped()

  
  //----------------------------------------------------------------------------
  // Constructor.
  ParticleListActionService::ParticleListActionService(fhicl::ParameterSet const & p,
						       art::ActivityRegistry &)
    : artg4tk::EventActionBase("PLASEventActionBase"),
      artg4tk::TrackingActionBase("PLASTrackingActionBase"),
      artg4tk::SteppingActionBase("PLASSteppingActionBase"),
      fenergyCut(p.get<double>("EnergyCut",0.0*CLHEP::GeV)),
      fstoreTrajectories( p.get<bool>("storeTrajectories",true)),
      fKeepEMShowerDaughters(p.get<bool>("keepEMShowerDaughters",true))
  {

    // Create the particle list that we'll (re-)use during the course
    // of the Geant4 simulation.
    fparticleList = new sim::ParticleList;
    fParentIDMap.clear();
  }

  art::Event  *ParticleListActionService::getCurrArtEvent() { return (currentArtEvent_); }
 //----------------------------------------------------------------------------
  // Destructor.
  ParticleListActionService::~ParticleListActionService()
  {
    // Delete anything that we created with "new'.
    delete fparticleList;
  }

  //----------------------------------------------------------------------------
  // Begin the event
  void ParticleListActionService::beginOfEventAction(const G4Event*)
  {
    // Clear any previous particle information.
    fCurrentParticle.clear();
    fparticleList->clear();
    fParentIDMap.clear();
    fCurrentTrackID = sim::NoParticleId;
   }

  //-------------------------------------------------------------
  // figure out the ultimate parentage of the particle with track ID
  // trackid
  // assume that the current track id has already been added to
  // the fParentIDMap
  int ParticleListActionService::GetParentage(int trackid) const
  {
    int parentid = sim::NoParticleId;
    
    // search the fParentIDMap recursively until we have the parent id 
    // of the first EM particle that led to this one
    std::map<int,int>::const_iterator itr = fParentIDMap.find(trackid);
    while( itr != fParentIDMap.end() ){
      LOG_DEBUG("ParticleListActionService")
      << "parentage for " << trackid
      << " " << (*itr).second;
      
      // set the parentid to the current parent ID, when the loop ends
      // this id will be the first EM particle 
      parentid = (*itr).second;
      itr = fParentIDMap.find(parentid);
    }
    LOG_DEBUG("ParticleListActionService") << "final parent ID " << parentid; 

    return parentid;
  }

  //----------------------------------------------------------------------------
  // Create our initial simb::MCParticle object and add it to the sim::ParticleList.
  void ParticleListActionService::preUserTrackingAction(const G4Track* track)
  {
     // Particle type.
    G4ParticleDefinition* particleDefinition = track->GetDefinition();
    G4int pdgCode = particleDefinition->GetPDGEncoding();

    // Get Geant4's ID number for this track.  This will be the same
    // ID number that we'll use in the ParticleList.
    // It is offset by the number of tracks accumulated from the previous Geant4
    // runs (if any)
    G4int trackID = track->GetTrackID() + fTrackIDOffset;
    fCurrentTrackID = trackID;

    // And the particle's parent (same offset as above):
    G4int parentID = track->GetParentID() + fTrackIDOffset;

    std::string process_name = "unknown";

    // Is there an MCTruth object associated with this G4Track?  We
    // have to go up a "chain" of information to find out:
    const G4DynamicParticle* dynamicParticle = track->GetDynamicParticle();
    const G4PrimaryParticle* primaryParticle = dynamicParticle->GetPrimaryParticle();
    if ( primaryParticle != 0 ){
      const G4VUserPrimaryParticleInformation* gppi = primaryParticle->GetUserInformation();
      const g4b::PrimaryParticleInformation* ppi = dynamic_cast<const g4b::PrimaryParticleInformation*>(gppi);
      if ( ppi != 0 ){
        // If we've made it this far, a PrimaryParticleInformation
        // object exists and we are using a primary particle, set the
        // process name accordingly
        process_name = "primary";
        
        // primary particles should have parentID = 0, even if there
        // are multiple MCTruths for this event
        parentID = 0;
      } // end else no primary particle information
    } // Is there a G4PrimaryParticle?
    // If this is not a primary particle...
    else{
      // check if this particle was made in an EM shower, don't put it in the particle
      // list as we don't care about secondaries, tertiaries, etc for these showers
      // figure out what process is making this track - skip it if it is
      // one of pair production, compton scattering, photoelectric effect
      // bremstrahlung, annihilation, any ionization - who wants to save
      // a buttload of electrons that arent from a CC interaction?
      process_name = track->GetCreatorProcess()->GetProcessName();
      if( !fKeepEMShowerDaughters
         && (process_name.find("conv")               != std::string::npos
             || process_name.find("LowEnConversion") != std::string::npos
             || process_name.find("Pair")            != std::string::npos
             || process_name.find("compt")           != std::string::npos
             || process_name.find("Compt")           != std::string::npos
             || process_name.find("Brem")            != std::string::npos
             || process_name.find("phot")            != std::string::npos
             || process_name.find("Photo")           != std::string::npos
             || process_name.find("Ion")             != std::string::npos
             || process_name.find("annihil")         != std::string::npos)
         ){
        
        // figure out the ultimate parentage of this particle
        // first add this track id and its parent to the fParentIDMap
        fParentIDMap[trackID] = parentID;
        
        fCurrentTrackID = -1*this->GetParentage(trackID);
        
        // check that fCurrentTrackID is in the particle list - it is possible
        // that this particle's parent is a particle that did not get tracked.
        // An example is a partent that was made due to muMinusCaptureAtRest
        // and the daughter was made by the phot process.  The parent likely
        // isn't saved in the particle list because it is below the energy cut
        // which will put a bogus track id value into the sim::IDE object for
        // the sim::SimChannel if we don't check it.
        if(!fparticleList->KnownParticle(fCurrentTrackID))
          fCurrentTrackID = sim::NoParticleId;
        
        // clear current particle as we are not stepping this particle and
        // adding trajectory points to it
        fCurrentParticle.clear();
        return;
        
      } // end if keeping EM shower daughters
      
      // Check the energy of the particle.  If it falls below the energy
      // cut, don't add it to our list.
      G4double energy = track->GetKineticEnergy();
      if( energy < fenergyCut ){
        fCurrentParticle.clear();
        
        // do add the particle to the parent id map though
        // and set the current track id to be it's ultimate parent
        fParentIDMap[trackID] = parentID;
        fCurrentTrackID = -1*this->GetParentage(trackID);
        
        return;
      }
      
      // check to see if the parent particle has been stored in the particle navigator
      // if not, then see if it is possible to walk up the fParentIDMap to find the
      // ultimate parent of this particle.  Use that ID as the parent ID for this
      // particle
      if( !fparticleList->KnownParticle(parentID) ){
        // do add the particle to the parent id map
        // just in case it makes a daughter that we have to track as well
        fParentIDMap[trackID] = parentID;
        int pid = this->GetParentage(parentID);
        
        // if we still can't find the parent in the particle navigator,
        // we have to give up
        if( !fparticleList->KnownParticle(pid) ){
          LOG_WARNING("ParticleListActionService")
          << "can't find parent id: "
          << parentID
          << " in the particle list, or fParentIDMap."
          << " Make " << parentID << " the mother ID for"
          << " track ID " << fCurrentTrackID
          << " in the hope that it will aid debugging.";
        }
        else
          parentID = pid;
      }
      
    }// end if not a primary particle
    
      // This is probably the PDG mass, but just in case:
    double mass = dynamicParticle->GetMass()/CLHEP::GeV;
    
      // Create the sim::Particle object.
    fCurrentParticle.clear();
    fCurrentParticle.particle    = new simb::MCParticle( trackID, pdgCode, process_name, parentID, mass);
      // if we are not filtering, we have a decision already
    if (!fFilter) fCurrentParticle.keep = true;
    
      // Polarization.
    const G4ThreeVector& polarization = track->GetPolarization();
    fCurrentParticle.particle->SetPolarization( TVector3( polarization.x(),
                                                         polarization.y(),
                                                         polarization.z() ) );
    
      // Save the particle in the ParticleList.
    fparticleList->Add( fCurrentParticle.particle );
  }

  //----------------------------------------------------------------------------
  void ParticleListActionService::postUserTrackingAction( const G4Track* aTrack)
  {
     if (!fCurrentParticle.hasParticle()) return;
    
    // if we have found no reason to keep it, drop it!
    // (we might still need parentage information though)
    if (!fCurrentParticle.keep) {
      fparticleList->Archive(fCurrentParticle.particle);
      // after the particle is archived, it is deleted
      fCurrentParticle.clear();
      return;
    }

    if(aTrack){
      fCurrentParticle.particle->SetWeight(aTrack->GetWeight());
      G4String process = aTrack->GetStep()->GetPostStepPoint()->GetProcessDefinedStep()->GetProcessName();
      fCurrentParticle.particle->SetEndProcess(process);
    }
    return;
  }
  
  
  //----------------------------------------------------------------------------
  // With every step, add to the particle's trajectory.
  void ParticleListActionService::userSteppingAction(const G4Step* step)
  {
     if ( !fCurrentParticle.hasParticle() ) {
      return;
    }

    // Temporary fix for problem where  DeltaTime on the first step
    // of optical photon propagation is calculated incorrectly. -wforeman
    globalTime = step->GetTrack()->GetGlobalTime();
    velocity_G4 = step->GetTrack()->GetVelocity();
    velocity_step = step->GetStepLength() / step->GetDeltaTime();
    if ( (step->GetTrack()->GetDefinition()->GetPDGEncoding()==0) &&
         fabs(velocity_G4 - velocity_step) > 0.0001 ) {
      // Subtract the faulty step time from the global time,
      // and add the correct step time based on G4 velocity.
      step->GetPostStepPoint()->SetGlobalTime(globalTime - step->GetDeltaTime() + step->GetStepLength()/velocity_G4);
    }


    // For the most part, we just want to add the post-step
    // information to the particle's trajectory.  There's one
    // exception: In PreTrackingAction, the correct time information
    // is not available.  So add the correct vertex information here.

    if ( fCurrentParticle.particle->NumberTrajectoryPoints() == 0 ){
      
      // Get the pre/along-step information from the G4Step.
      const G4StepPoint* preStepPoint = step->GetPreStepPoint();
      
      const G4ThreeVector position = preStepPoint->GetPosition();
      G4double time = preStepPoint->GetGlobalTime();
      
      // Remember that LArSoft uses cm, ns, GeV.
      TLorentzVector fourPos(position.x() / CLHEP::cm,
                             position.y() / CLHEP::cm,
                             position.z() / CLHEP::cm,
                             time / CLHEP::ns);
      
      const G4ThreeVector momentum = preStepPoint->GetMomentum();
      const G4double energy = preStepPoint->GetTotalEnergy();
      TLorentzVector fourMom(momentum.x() / CLHEP::GeV,
                             momentum.y() / CLHEP::GeV,
                             momentum.z() / CLHEP::GeV,
                             energy / CLHEP::GeV);
      
      // Add the first point in the trajectory.
      AddPointToCurrentParticle( fourPos, fourMom, "Start" );
      
    } // end if this is the first step

    // At this point, the particle is being transported through the
    // simulation. This method is being called for every voxel that
    // the track passes through, but we don't want to update the
    // trajectory information if we're just updating voxels. To check
    // for this, look at the process name for the step, and compare it
    // against the voxelization process name (set in PhysicsList.cxx).
    G4String process = step->GetPostStepPoint()->GetProcessDefinedStep()->GetProcessName();
    G4bool ignoreProcess = process.contains("LArVoxel") || process.contains("OpDetReadout"); 

    LOG_DEBUG("ParticleListActionService::SteppingAction")
    << ": DEBUG - process='"
    << process << "'"
    << " ignoreProcess=" << ignoreProcess
    << " fstoreTrajectories="
    << fstoreTrajectories;
    
    // We store the initial creation point of the particle
    // and its final position (ie where it has no more energy, or at least < 1 eV) no matter
    // what, but whether we store the rest of the trajectory depends
    // on the process, and on a user switch.
    if ( fstoreTrajectories  &&  !ignoreProcess ){
      // Get the post-step information from the G4Step.
      const G4StepPoint* postStepPoint = step->GetPostStepPoint();
      
      const G4ThreeVector position = postStepPoint->GetPosition();
      G4double time = postStepPoint->GetGlobalTime();
      
      // Remember that LArSoft uses cm, ns, GeV.
      TLorentzVector fourPos( position.x() / CLHEP::cm,
                             position.y() / CLHEP::cm,
                             position.z() / CLHEP::cm,
                             time / CLHEP::ns );
      
      const G4ThreeVector momentum = postStepPoint->GetMomentum();
      const G4double energy = postStepPoint->GetTotalEnergy();
      TLorentzVector fourMom( momentum.x() / CLHEP::GeV,
                             momentum.y() / CLHEP::GeV,
                             momentum.z() / CLHEP::GeV,
                             energy / CLHEP::GeV );
      
      // Add another point in the trajectory.
      AddPointToCurrentParticle( fourPos, fourMom, std::string(process) );
     }
  }

  //----------------------------------------------------------------------------
  /// Utility class for the EndOfEventAction method: update the
  /// daughter relationships in the particle list.
  class UpdateDaughterInformation
    : public std::unary_function<sim::ParticleList::value_type, void>
  {
  public:
    UpdateDaughterInformation()
      : particleList(0)
    {}
    void SetParticleList( sim::ParticleList* p ) { particleList = p; }
    void operator()( sim::ParticleList::value_type& particleListEntry )
    {
      // We're looking at this Particle in the list.
      int particleID = particleListEntry.first;

      // The parent ID of this particle;
      // we ask the particle list since the particle itself might have been lost
      // ("archived"), but the particle list still holds the information we need
      int parentID = particleList->GetMotherOf(particleID);
      
      // If the parentID <= 0, this is a primary particle.
      if ( parentID <= 0 ) return;

      // If we get here, this particle is somebody's daughter.  Add
      // it to the list of daughter particles for that parent.

      // Get the parent particle from the list.
      sim::ParticleList::iterator parentEntry = particleList->find( parentID );

      if ( parentEntry == particleList->end() ){
        // We have an "orphan": a particle whose parent isn't
        // recorded in the particle list.  This is not signficant;
        // it's possible for a particle not to be saved in the list
        // because it failed an energy cut, but for it to have a
        // daughter that passed the cut (e.g., a nuclear decay).
        return;
      }
      if ( !parentEntry->second ) return; // particle archived, nothing to update

      // Add the current particle to the daughter list of the
      // parent.
      simb::MCParticle* parent = (*parentEntry).second;
      parent->AddDaughter( particleID );
    }
  private:
    sim::ParticleList* particleList;     
  };

  //----------------------------------------------------------------------------
  // Returns the ParticleList accumulated during the current event.
  const sim::ParticleList* ParticleListActionService::GetList() const
  {
    // check if the ParticleNavigator has entries, and if
    // so grab the highest track id value from it to 
    // add to the fTrackIDOffset
    int highestID = 0;
    for( auto pn = fparticleList->begin(); pn != fparticleList->end(); pn++)
      if( (*pn).first > highestID ) highestID = (*pn).first;
      
    //Only change the fTrackIDOffset if there is in fact a particle to add to the event
    if( (fparticleList->size())!=0){ fTrackIDOffset = highestID + 1; }

    return fparticleList;
  }
 
  //----------------------------------------------------------------------------
  // Yields the ParticleList accumulated during the current event.
  sim::ParticleList&& ParticleListActionService::YieldList()
  {
    // check if the ParticleNavigator has entries, and if
    // so grab the highest track id value from it to 
    // add to the fTrackIDOffset
    int highestID = 0;
    for( auto pn = fparticleList->begin(); pn != fparticleList->end(); pn++)
      if( (*pn).first > highestID ) highestID = (*pn).first;
    
    //Only change the fTrackIDOffset if there is in fact a particle to add to the event
    if( (fparticleList->size())!=0 ){ fTrackIDOffset = highestID + 1; }

    return std::move(*fparticleList);
  } // ParticleList&& ParticleListActionService::YieldList()
  
  
  //----------------------------------------------------------------------------
  void ParticleListActionService::AddPointToCurrentParticle(TLorentzVector const& pos,
                                                     TLorentzVector const& mom,
                                                     std::string    const& process)
  {
    // Add the first point in the trajectory.
    fCurrentParticle.particle->AddTrajectoryPoint(pos, mom, process);
    
    // also see if we can decide to keep the particle
    if (!fCurrentParticle.keep)
        fCurrentParticle.keep = fFilter->mustKeep(pos);
    
  } // ParticleListActionService::AddPointToCurrentParticle()
  
// Called at the end of each event. Call detectors to convert hits for the 
// event and pass the call on to the action objects.
  void ParticleListActionService::endOfEventAction(const G4Event*)
{
  partCol_ = std::make_unique<std::vector<simb::MCParticle > >();
  tpassn_ = std::make_unique<art::Assns<simb::MCTruth, simb::MCParticle >>();
  // Set up the utility class for the "for_each" algorithm.  (We only
  // need a separate set-up for the utility class because we need to
  // give it the pointer to the particle list.  We're using the STL
  // "for_each" instead of the C++ "for loop" because it's supposed
  // to be faster.
  UpdateDaughterInformation updateDaughterInformation;
  updateDaughterInformation.SetParticleList( fparticleList );
  // Update the daughter information for each particle in the list.
  std::for_each(fparticleList->begin(), 
		fparticleList->end(), 
		updateDaughterInformation);
  art::ServiceHandle<ActionHolderService> ahs;
  sim::ParticleList particleList = YieldList();
  art::Event * evt= getCurrArtEvent();
  std::vector< art::Handle< std::vector<simb::MCTruth> > > mclists;
  evt->getManyByType(mclists);
  for(size_t mcl = 0; mcl < mclists.size(); ++mcl){
    art::Handle< std::vector<simb::MCTruth> > mclistHandle = mclists[mcl];
    for(size_t m = 0; m < mclistHandle->size(); ++m){
      art::Ptr<simb::MCTruth> mct(mclistHandle, m);
      unsigned int nGeneratedParticles = 0;
        auto iPartPair = particleList.begin();
        while (iPartPair != particleList.end()) {
          simb::MCParticle& p = *(iPartPair->second);
          ++nGeneratedParticles;         
          partCol_->push_back(std::move(p));
	  art::Ptr<simb::MCParticle> mcp_ptr = art::Ptr<simb::MCParticle>(pid_,partCol_->size()-1,evt->productGetter(pid_));
          tpassn_->addSingle(mct, mcp_ptr);
        } // while(particleList)
    }
  }
    // Every ACTION needs to write out their event data now
  ahs -> fillEventWithArtStuff();
}
} // namespace LArG4
using larg4::ParticleListActionService;
DEFINE_ART_SERVICE(ParticleListActionService)
