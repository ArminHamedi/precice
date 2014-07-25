// Copyright (C) 2011 Technische Universitaet Muenchen
// This file is part of the preCICE project. For conditions of distribution and
// use, please see the license notice at http://www5.in.tum.de/wiki/index.php/PreCICE_License
#include "BaseCouplingScheme.hpp"
#include "CompositionalCouplingScheme.hpp"
#include "mesh/Mesh.hpp"
#include "com/Communication.hpp"
#include "utils/Globals.hpp"
#include "impl/PostProcessing.hpp"
#include "io/TXTWriter.hpp"
#include "io/TXTReader.hpp"
#include <limits>

namespace precice {
namespace cplscheme {

tarch::logging::Log BaseCouplingScheme::
_log("precice::cplscheme::BaseCouplingScheme");

BaseCouplingScheme:: BaseCouplingScheme
(
  double maxTime,
  int    maxTimesteps,
  double timestepLength,
  int    validDigits)
  :
  _maxTime(maxTime),
  _maxTimesteps(maxTimesteps),
  _timestepLength(timestepLength),
  _doesFirstStep(false),
  _validDigits(validDigits),
  _time(0.0),
  _computedTimestepPart(0.0),
  _timesteps(0),
  _checkpointTimestepInterval(-1),
  _isCouplingOngoing(true),
  _isCouplingTimestepComplete(false),
  _hasDataBeenExchanged(false),
  _hasToReceiveInitData(false),
  _hasToSendInitData(false),
  _isInitialized(false),
  _actions(),
  _sendData(),
  _receiveData (),
  _iterationsWriter("iterations-unknown.txt")
{
  preciceCheck (
    not ((maxTime != UNDEFINED_TIME) && (maxTime < 0.0)),
    "BaseCouplingScheme()", "Maximum time has to be larger than zero!");
  preciceCheck (
    not ((maxTimesteps != UNDEFINED_TIMESTEPS) && (maxTimesteps < 0)),
    "BaseCouplingScheme()", "Maximum timestep number has to be larger than zero!");
  preciceCheck (
    not ((timestepLength != UNDEFINED_TIMESTEP_LENGTH) && (timestepLength < 0.0)),
    "BaseCouplingScheme()", "Timestep length has to be larger than zero!");
  preciceCheck((_validDigits >= 1) && (_validDigits < 17),
	       "BaseCouplingScheme()", "Valid digits of timestep length has to be "
	       << "between 1 and 16!");
}

BaseCouplingScheme::BaseCouplingScheme
(
  double                maxTime,
  int                   maxTimesteps,
  double                timestepLength,
  int                   validDigits,
  const std::string&    firstParticipant,
  const std::string&    secondParticipant,
  const std::string&    localParticipant,
  com::PtrCommunication communication,
  int                   maxIterations,
  constants::TimesteppingMethod dtMethod )
  :
  _maxTime(maxTime),
   _maxTimesteps(maxTimesteps),
   _timestepLength(timestepLength),
  _doesFirstStep(false),
  _validDigits(validDigits),
  _time(0.0),
  _computedTimestepPart(0.0),
  _timesteps(0),
  _checkpointTimestepInterval(-1),
  _isCouplingOngoing(true),
  _isCouplingTimestepComplete(false),
  _hasDataBeenExchanged(false),
  _isInitialized(false),
  _actions(),
  _sendData(),
  _receiveData(),
  _firstParticipant(firstParticipant),
  _secondParticipant(secondParticipant),
  _communication(communication),
  _iterationsWriter("iterations-" + localParticipant + ".txt"),
//_residualWriterL1("residualL1-" + localParticipant + ".txt"),
//_residualWriterL2("residualL2-" + localParticipant + ".txt"),
//_amplificationWriter("amplification-" + localParticipant + ".txt"),
  _convergenceMeasures(),
  _postProcessing(),
  _extrapolationOrder(0),
  _maxIterations(maxIterations),
  _iterationToPlot(0),
  _timestepToPlot(0),
  _timeToPlot(0.0),
  _iterations(0),
  _totalIterations(0),
  _participantSetsDt(false),
  _participantReceivesDt(false),
  _hasToReceiveInitData(false),
  _hasToSendInitData(false)
{
  preciceCheck(
    not ((maxTime != UNDEFINED_TIME) && (maxTime < 0.0)),
    "BaseCouplingScheme()", "Maximum time has to be larger than zero!");
  preciceCheck(
    not ((maxTimesteps != UNDEFINED_TIMESTEPS) && (maxTimesteps < 0)),
    "BaseCouplingScheme()", "Maximum timestep number has to be larger than zero!");
  preciceCheck(
    not ((timestepLength != UNDEFINED_TIMESTEP_LENGTH) && (timestepLength < 0.0)),
    "BaseCouplingScheme()", "Timestep length has to be larger than zero!");
  preciceCheck((_validDigits >= 1) && (_validDigits < 17),
	       "BaseCouplingScheme()", "Valid digits of timestep length has to be "
	       << "between 1 and 16!");
  preciceCheck(_firstParticipant != _secondParticipant,
	       "ImplicitCouplingScheme()", "First participant and "
	       << "second participant must have different names! Called from BaseCoupling.");
  if (dtMethod == constants::FIXED_DT){
    preciceCheck(hasTimestepLength(), "ImplicitCouplingScheme()",
		 "Timestep length value has to be given "
		 << "when the fixed timestep length method is chosen for an implicit "
		 << "coupling scheme!");
  }
  if (localParticipant == _firstParticipant){
    _doesFirstStep = true;
    if (dtMethod == constants::FIRST_PARTICIPANT_SETS_DT){
      _participantSetsDt = true;
      setTimestepLength(UNDEFINED_TIMESTEP_LENGTH);
    }
  }
  else if (localParticipant == _secondParticipant){
    if (dtMethod == constants::FIRST_PARTICIPANT_SETS_DT){
      _participantReceivesDt = true;
    }
  }
  else {
    preciceError("initialize()", "Name of local participant \""
		 << localParticipant << "\" does not match any "
		 << "participant specified for the coupling scheme!");
  }
  preciceCheck((maxIterations > 0) || (maxIterations == -1),
	       "ImplicitCouplingState()",
	       "Maximal iteration limit has to be larger than zero!");
  assertion(_communication.use_count() > 0);
}


// temp function to make refactoring clearer
void BaseCouplingScheme::receiveAndSetDt()
{
  if (participantReceivesDt()){
    double dt = UNDEFINED_TIMESTEP_LENGTH;
    getCommunication()->receive(dt, 0);
    assertion(not tarch::la::equals(dt, UNDEFINED_TIMESTEP_LENGTH));
    setTimestepLength(dt);
  }
}


void BaseCouplingScheme:: addDataToSend
(
  mesh::PtrData data,
  bool          initialize)
{
  int id = data->getID();
  if(! utils::contained(id, _sendData)) {
    PtrCouplingData ptrCplData (new CouplingData(& (data->values()), initialize));
    DataMap::value_type pair = std::make_pair (id, ptrCplData);
    _sendData.insert(pair);
  }
  else {
    preciceError("addDataToSend()", "Data \"" << data->getName()
		 << "\" of mesh \"" << data->mesh()->getName() << "\" cannot be "
		 << "added twice for sending!");
  }
}

void BaseCouplingScheme:: addDataToReceive
(
  mesh::PtrData data,
  bool          initialize)
{
  int id = data->getID();
  if(! utils::contained(id, _receiveData)) {
    PtrCouplingData ptrCplData (new CouplingData(& (data->values()), initialize));
    DataMap::value_type pair = std::make_pair (id, ptrCplData);
    _receiveData.insert(pair);
  }
  else {
    preciceError("addDataToReceive()", "Data \"" << data->getName()
		 << "\" of mesh \"" << data->mesh()->getName() << "\" cannot be "
		 << "added twice for receiving!");
  }
}


void BaseCouplingScheme:: sendState
(
  com::PtrCommunication communication,
  int                   rankReceiver)
{
  preciceTrace1("sendState()", rankReceiver);
  communication->startSendPackage(rankReceiver );
  assertion(communication.get() != NULL);
  assertion(communication->isConnected());
  communication->send(_maxTime, rankReceiver);
  communication->send(_maxTimesteps, rankReceiver);
  communication->send(_timestepLength, rankReceiver);
  communication->send(_time, rankReceiver);
  communication->send(_timesteps, rankReceiver);
  communication->send(_checkpointTimestepInterval, rankReceiver);
  communication->send(_computedTimestepPart, rankReceiver);
  //communication->send(_maxLengthNextTimestep, rankReceiver);
  communication->send(_isInitialized, rankReceiver);
  communication->send(_isCouplingTimestepComplete, rankReceiver);
  communication->send(_hasDataBeenExchanged, rankReceiver);
  communication->send((int)_actions.size(), rankReceiver);
  foreach(const std::string& action, _actions) {
    communication->send(action, rankReceiver);
  }
  communication->send(_maxIterations, rankReceiver );
  communication->send(_iterations, rankReceiver );
  communication->send(_totalIterations, rankReceiver );
  communication->finishSendPackage();

}

void BaseCouplingScheme:: receiveState
(
  com::PtrCommunication communication,
  int                   rankSender)
{
  preciceTrace1("receiveState()", rankSender);
  communication->startReceivePackage(rankSender);
  assertion(communication.get() != NULL);
  assertion(communication->isConnected());
  communication->receive(_maxTime, rankSender);
  communication->receive(_maxTimesteps, rankSender);
  communication->receive(_timestepLength, rankSender);
  communication->receive(_time, rankSender);
  communication->receive(_timesteps, rankSender);
  communication->receive(_checkpointTimestepInterval, rankSender);
  communication->receive(_computedTimestepPart, rankSender);
  //communication->receive(_maxLengthNextTimestep, rankSender);
  communication->receive(_isInitialized, rankSender);
  communication->receive(_isCouplingTimestepComplete, rankSender);
  communication->receive(_hasDataBeenExchanged, rankSender);
  int actionsSize = 0;
  communication->receive(actionsSize, rankSender);
  _actions.clear();
  for (int i=0; i < actionsSize; i++) {
    std::string action;
    communication->receive(action, rankSender);
    _actions.insert(action);
  }
  communication->receive(_maxIterations, rankSender);
  int subIteration = -1;
  communication->receive(subIteration, rankSender);
  _iterations = subIteration;
  communication->receive(_totalIterations, rankSender);
  communication->finishReceivePackage();

}

std::vector<int> BaseCouplingScheme:: sendData
(
  com::PtrCommunication communication)
{
  preciceTrace("sendData()");
  assertion(communication.get() != NULL);
  assertion(communication->isConnected());

  std::vector<int> sentDataIDs;
  foreach (DataMap::value_type& pair, _sendData){
    int size = pair.second->values->size ();
    if (size > 0){
      communication->send(tarch::la::raw(*pair.second->values), size, 0);
    }
    sentDataIDs.push_back(pair.first);
  }
  preciceDebug("Number of sent data sets = " << sentDataIDs.size());
  return sentDataIDs;
}

std::vector<int> BaseCouplingScheme:: receiveData
(
  com::PtrCommunication communication)
{
  preciceTrace("receiveData()");
  assertion(communication.get() != NULL);
  assertion(communication->isConnected());

  std::vector<int> receivedDataIDs;
  foreach(DataMap::value_type & pair, _receiveData){
    int size = pair.second->values->size ();
    if (size > 0){
      communication->receive(tarch::la::raw(*pair.second->values), size, 0);
    }
    receivedDataIDs.push_back(pair.first);
  }
  preciceDebug("Number of received data sets = " << receivedDataIDs.size());
  return receivedDataIDs;
}

CouplingData* BaseCouplingScheme:: getSendData
(
  int dataID)
{
  preciceTrace1("getSendData()", dataID);
  DataMap::iterator iter = _sendData.find(dataID);
  if(iter != _sendData.end()){
    return  &(*(iter->second));
  }
  return NULL;
}

CouplingData* BaseCouplingScheme:: getReceiveData
(
  int dataID)
{
  preciceTrace1("getReceiveData()", dataID);
  DataMap::iterator iter = _receiveData.find(dataID);
  if(iter != _receiveData.end()){
    return  &(*(iter->second));
  }
  return NULL;
}

void BaseCouplingScheme::finalize()
{
  preciceTrace("finalize()");
  checkCompletenessRequiredActions();
  preciceCheck(isInitialized(), "finalize()",
	       "Called finalize() before initialize()!");
  preciceCheck(not isCouplingOngoing(), "finalize()",
	       "Called finalize() while isCouplingOngoing() returns true!");
}

void BaseCouplingScheme:: setExtrapolationOrder
(
  int order)
{
  preciceCheck((order == 0) || (order == 1) || (order == 2),
               "setExtrapolationOrder()", "Extrapolation order has to be "
               << " 0, 1, or 2!");
  _extrapolationOrder = order;
}


bool BaseCouplingScheme:: hasTimestepLength() const
{
  return not tarch::la::equals(_timestepLength, UNDEFINED_TIMESTEP_LENGTH);
}

double BaseCouplingScheme:: getTimestepLength() const
{
  assertion(not tarch::la::equals(_timestepLength, UNDEFINED_TIMESTEP_LENGTH));
  return _timestepLength;
}

void BaseCouplingScheme:: addComputedTime
(
  double timeToAdd )
{
  preciceTrace2("addComputedTime()", timeToAdd, _time);
  preciceCheck(isCouplingOngoing(), "addComputedTime()",
	       "Invalid call of addComputedTime() after simulation end!");

  _computedTimestepPart += timeToAdd;
  _time += timeToAdd;

  // Check validness
  double eps = std::pow(10.0, -1 * _validDigits);
  bool valid = tarch::la::greaterEquals(getThisTimestepRemainder(), 0.0, eps);
  preciceCheck(valid, "addComputedTime()", "The computed timestep length of "
	       << timeToAdd << " exceeds the maximum timestep limit of "
	       << _timestepLength - _computedTimestepPart + timeToAdd
	       << " for this time step!");
}

bool BaseCouplingScheme:: willDataBeExchanged
(
  double lastSolverTimestepLength) const
{
  preciceTrace1("willDataBeExchanged()", lastSolverTimestepLength);
  double eps = std::pow(10.0, -1 * _validDigits);
  double remainder = getThisTimestepRemainder() - lastSolverTimestepLength;
  return not tarch::la::greater(remainder, 0.0, eps);
}

bool BaseCouplingScheme:: hasDataBeenExchanged() const
{
  return _hasDataBeenExchanged;
}

void BaseCouplingScheme:: setHasDataBeenExchanged
(
  bool hasDataBeenExchanged)
{
  _hasDataBeenExchanged = hasDataBeenExchanged;
}

double BaseCouplingScheme:: getTime() const
{
  return _time;
}

int BaseCouplingScheme:: getTimesteps() const
{
  return _timesteps;
}


std::vector<std::string> BaseCouplingScheme::getCouplingPartners() const
{
  std::vector<std::string> partnerNames;

  // Add non-local participant
  if(doesFirstStep()){
    partnerNames.push_back(_secondParticipant);
  }
  else {
    partnerNames.push_back(_firstParticipant);
  }
  return partnerNames;
}


double BaseCouplingScheme:: getThisTimestepRemainder() const
{
  preciceTrace("getTimestepRemainder()");
  double remainder = 0.0;
  if (not tarch::la::equals(_timestepLength, UNDEFINED_TIMESTEP_LENGTH)){
    remainder = _timestepLength - _computedTimestepPart;
  }
  preciceDebug("return " << remainder);
  return remainder;
}

double BaseCouplingScheme:: getNextTimestepMaxLength() const
{
  if (tarch::la::equals(_timestepLength, UNDEFINED_TIMESTEP_LENGTH)){
    if (tarch::la::equals(_maxTime, UNDEFINED_TIME)){
      return std::numeric_limits<double>::max();
    }
    else {
      return _maxTime - _time;
    }
  }
  return _timestepLength - _computedTimestepPart;
}

bool BaseCouplingScheme:: isCouplingOngoing() const
{
  double eps = std::pow(10.0, -1 * _validDigits);
  using namespace tarch::la;
  bool timeLeft = greater(_maxTime, _time, eps) || equals(_maxTime, UNDEFINED_TIME);
  bool timestepsLeft = (_maxTimesteps > _timesteps)
    || (_maxTimesteps == UNDEFINED_TIMESTEPS);
  return timeLeft && timestepsLeft;
}

bool BaseCouplingScheme:: isCouplingTimestepComplete() const
{
  return _isCouplingTimestepComplete;
}

bool BaseCouplingScheme:: isActionRequired
(
  const std::string& actionName) const
{
  return _actions.count(actionName) > 0;
}

void BaseCouplingScheme:: performedAction
(
  const std::string& actionName)
{
  _actions.erase(actionName);
}

int BaseCouplingScheme:: getCheckpointTimestepInterval() const
{
  return _checkpointTimestepInterval;
}

void BaseCouplingScheme:: requireAction
(
  const std::string& actionName)
{
  _actions.insert(actionName);
}

std::string BaseCouplingScheme:: printBasicState() const
{
  std::ostringstream os;
  os << printBasicState(_timesteps, _time);
  return os.str ();
}

std::string BaseCouplingScheme:: printBasicState
(
  int    timesteps,
  double time ) const
{
  std::ostringstream os;
  os << "dt# " << timesteps;
  if(_maxTimesteps != UNDEFINED_TIMESTEPS){
    os << " of " << _maxTimesteps;
  }
  os << " | t " << time;
  if(_maxTime != UNDEFINED_TIME){
    os << " of " << _maxTime;
  }
  if(_timestepLength != UNDEFINED_TIMESTEP_LENGTH){
    os << " | dt " << _timestepLength;
  }
  if((_timestepLength != UNDEFINED_TIMESTEP_LENGTH)
     || (_maxTime != UNDEFINED_TIME))
  {
    os << " | max dt " << getNextTimestepMaxLength();
  }
  os << " | ongoing ";
  isCouplingOngoing() ? os << "yes" : os << "no";
  os << " | dt complete ";
  _isCouplingTimestepComplete ? os << "yes" : os << "no";
  return os.str ();
}

std::string BaseCouplingScheme:: printActionsState () const
{
  std::ostringstream os;
  foreach(const std::string & actionName, _actions) {
    os << actionName << " | ";
  }
  return os.str ();
}

void BaseCouplingScheme:: checkCompletenessRequiredActions ()
{
  preciceTrace("checkCompletenessRequiredActions()");
  if(not _actions.empty()){
    std::ostringstream stream;
    foreach(const std::string & action, _actions){
      if (not stream.str().empty()){
	stream << ", ";
      }
      stream << action;
    }
    preciceError("checkCompletenessRequiredActions()",
		 "Unfulfilled required actions: " << stream.str() << "!");
  }
}

int BaseCouplingScheme:: getValidDigits () const
{
  return _validDigits;
}

void BaseCouplingScheme::initialize
(
  double startTime,
  int    startTimestep)
{
  preciceTrace2("initialize()", startTime, startTimestep);
  assertion(not isInitialized());
  assertion1(tarch::la::greaterEquals(startTime, 0.0), startTime);
  assertion1(startTimestep >= 0, startTimestep);
  assertion(getCommunication()->isConnected());
  // This currently does not fail, though description suggests it should in some cases for explicit coupling. 
  preciceCheck(not getSendData().empty(), "initialize()",
	       "No send data configured! Use explicit scheme for one-way coupling.");
  setTime(startTime);
  setTimesteps(startTimestep);

  if (not doesFirstStep()){
    if (not _convergenceMeasures.empty()) {
      setupConvergenceMeasures(); // needs _couplingData configured
      setupDataMatrices(getSendData()); // Reserve memory and initialize data with zero
    }
    if (getPostProcessing().get() != NULL){
      preciceCheck(getPostProcessing()->getDataIDs().size()==1 ,"initialize()",
		   "For serial coupling, the number of coupling data vectors has to be 1");
      getPostProcessing()->initialize(getSendData()); // Reserve memory, initialize
    }
  }
  else if (getPostProcessing().get() != NULL){
    int dataID = *(getPostProcessing()->getDataIDs().begin());
    preciceCheck(getSendData(dataID) == NULL, "initialize()",
		 "In case of serial coupling, post-processing can be defined for "
		 << "data of second participant only!");
  }

  // This test is valid, if only implicit schemes have convergence measures.
  // It currently holds, we will maybe find something better
  if (not _convergenceMeasures.empty()) {
    requireAction(constants::actionWriteIterationCheckpoint());
  }
    
  foreach (DataMap::value_type & pair, getSendData()){
    if (pair.second->initialize){
      preciceCheck(not doesFirstStep(), "initialize()",
		   "Only second participant can initialize data!");
      preciceDebug("Initialized data to be written");
      setHasToSendInitData(true);
      break;
    }
  }

  foreach (DataMap::value_type & pair, getReceiveData()){
    if (pair.second->initialize){
      preciceCheck(doesFirstStep(), "initialize()",
		   "Only first participant can receive initial data!");
      preciceDebug("Initialized data to be received");
      setHasToReceiveInitData(true);
    }
  }

  
  // If the second participant initializes data, the first receive for the
  // second participant is done in initializeData() instead of initialize().
  if ((not doesFirstStep()) && (not hasToSendInitData()) && isCouplingOngoing()){
    preciceDebug("Receiving data");
    getCommunication()->startReceivePackage(0);
    if (participantReceivesDt()){
      double dt = UNDEFINED_TIMESTEP_LENGTH;
      getCommunication()->receive(dt, 0);
      preciceDebug("received timestep length of " << dt);
      assertion(not tarch::la::equals(dt, UNDEFINED_TIMESTEP_LENGTH));
      setTimestepLength(dt);
    }
    receiveData(getCommunication());
    getCommunication()->finishReceivePackage();
    setHasDataBeenExchanged(true);
  }

  if(hasToSendInitData()){
    requireAction(constants::actionWriteInitialData());
  }
  
  initializeTXTWriters();
  setIsInitialized(true);
}

void BaseCouplingScheme::initializeData()
{
  preciceTrace("initializeData()");
  preciceCheck(isInitialized(), "initializeData()",
	       "initializeData() can be called after initialize() only!");

  if((not hasToSendInitData()) && (not hasToReceiveInitData())){
    preciceInfo("initializeData()", "initializeData is skipped since no data has to be initialized");
    return;
  }

  preciceDebug("Initializing Data ...");
  
  preciceCheck(not (hasToSendInitData() && isActionRequired(constants::actionWriteInitialData())),
	       "initializeData()", "InitialData has to be written to preCICE before calling initializeData()");

  setHasDataBeenExchanged(false);

  if (hasToReceiveInitData() && isCouplingOngoing()){
    assertion(_doesFirstStep);
    preciceDebug("Receiving data");
    getCommunication()->startReceivePackage(0);
    if (participantReceivesDt()){
      double dt = UNDEFINED_TIMESTEP_LENGTH;
      getCommunication()->receive(dt, 0);
      preciceDebug("received timestep length of " << dt);
      assertion(not tarch::la::equals(dt, UNDEFINED_TIMESTEP_LENGTH));
      setTimestepLength(dt);
      //setMaxLengthNextTimestep(dt);
    }
    receiveData(getCommunication());
    getCommunication()->finishReceivePackage();
    setHasDataBeenExchanged(true);
  }


  if (hasToSendInitData() && isCouplingOngoing()){
    assertion(not _doesFirstStep);
    foreach (DataMap::value_type & pair, getSendData()){
      if (pair.second->oldValues.cols() == 0)
	break;
      utils::DynVector& oldValues = pair.second->oldValues.column(0);
      oldValues = *pair.second->values;

      // For extrapolation, treat the initial value as old timestep value
      pair.second->oldValues.shiftSetFirst(*pair.second->values);
    }

    // The second participant sends the initialized data to the first particpant
    // here, which receives the data on call of initialize().
    sendData(getCommunication());
    getCommunication()->startReceivePackage(0);
    // This receive replaces the receive in initialize().
    receiveData(getCommunication());
    getCommunication()->finishReceivePackage();
    setHasDataBeenExchanged(true);
  }

  //in order to check in advance if initializeData has been called (if necessary)
  setHasToSendInitData(false);
  setHasToReceiveInitData(false);
}

  
void BaseCouplingScheme::setupDataMatrices(DataMap& data)
{
  preciceTrace("setupDataMatrices()");
  preciceDebug("Data size: " << data.size());
  // Reserve storage for convergence measurement of send and receive data values
  foreach (ConvergenceMeasure& convMeasure, _convergenceMeasures){
    assertion(convMeasure.data != NULL);
    if (convMeasure.data->oldValues.cols() < 1){
      convMeasure.data->oldValues.append(CouplingData::DataMatrix(
					   convMeasure.data->values->size(), 1, 0.0));
    }
  }
  // Reserve storage for extrapolation of data values
  if (_extrapolationOrder > 0){
    foreach (DataMap::value_type& pair, data){
      int cols = pair.second->oldValues.cols();
      preciceDebug("Add cols: " << pair.first << ", cols: " << cols);
      assertion1(cols <= 1, cols);
      pair.second->oldValues.append(CouplingData::DataMatrix(
				      pair.second->values->size(), _extrapolationOrder + 1 - cols, 0.0));
    }
  }
}

void BaseCouplingScheme::setupConvergenceMeasures()
{
  preciceTrace("setupConvergenceMeasures()");
  assertion(not doesFirstStep());
  preciceCheck(not _convergenceMeasures.empty(), "setupConvergenceMeasures()",
	       "At least one convergence measure has to be defined for "
	       << "an implicit coupling scheme!");
  foreach (ConvergenceMeasure& convMeasure, _convergenceMeasures){
    int dataID = convMeasure.dataID;
    if ((getSendData(dataID) != NULL)){
      convMeasure.data = getSendData(dataID);
    }
    else {
      convMeasure.data = getReceiveData(dataID);
      assertion(convMeasure.data != NULL);
    }
  }
}

void BaseCouplingScheme::initializeTXTWriters()
{
  _iterationsWriter.addData("Timesteps", io::TXTTableWriter::INT );
  _iterationsWriter.addData("Total Iterations", io::TXTTableWriter::INT );
  _iterationsWriter.addData("Iterations", io::TXTTableWriter::INT );
  _iterationsWriter.addData("Convergence", io::TXTTableWriter::INT );
}
  

}} // namespace precice, cplscheme
