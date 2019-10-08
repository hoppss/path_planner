#include <thread>
#include <fstream>
#include <wait.h>
#include <future>
#include "ExecutiveInternalsManager.h"
#include "executive.h"
#include "../planner/SamplingBasedPlanner.h"
#include "../planner/AStarPlanner.h"
#include "../common/map/GeoTiffMap.h"
#include "../common/map/GridWorldMap.h"

using namespace std;

Executive::Executive(TrajectoryPublisher *controlReceiver)
{
    m_TrajectoryPublisher = controlReceiver;

    startThreads();
}

Executive::~Executive() {
    terminate();
    m_PlanningFuture.wait_for(chrono::seconds(1));
    m_TrajectoryPublishingFuture.wait_for(chrono::seconds(1));
}

double Executive::getCurrentTime()
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void Executive::updateCovered(double x, double y, double speed, double heading, double t)
{
//    m_InternalsManager.updateCurrent(x, y, speed, heading, t);
//    m_InternalsManager.updateCovered();
    if ((m_LastHeading - heading) / m_LastUpdateTime <= c_CoverageHeadingRateMax) {
        m_RibbonManager.cover(x, y);
    }
    m_LastUpdateTime = t; m_LastHeading = heading;
    m_LastState = State(x, y, heading, speed, t);
}

void Executive::sendAction() {
    int sleep = 50; // for 20 Hz
    while (m_Running)
    {
        // if m_Pause, block until !m_Pause
        unique_lock<mutex> lk(m_PauseMutex);
        m_PauseCv.wait(lk, [=]{return !m_Pause;});
//        cerr << "sendAction unblocked (with the lock)" << endl;
        lk.unlock();
//        cerr << "sendAction released the lock" << endl;
        auto actions = m_InternalsManager.getActions();
//        cerr << "sendAction got path actions" << endl;
        if (actions.empty())
            continue;
//        cerr << "and they aren't null" << endl;
        m_TrajectoryPublisher->publishTrajectory(actions);
//        cerr << "sendAction published trajectory" << endl;
        this_thread::sleep_for(std::chrono::milliseconds(sleep));

//        cerr << "sendAction asking if planner is dead" << endl;
//        if (plannerIsDead()) pause();
    }
}

// fix the moving of start
void Executive::requestPath()
{
    double start, end;
    int sleeptime;

//    m_InternalsManager.initialize();

    while (m_Running)
    {
        // if m_Pause, block until !m_Pause
        unique_lock<mutex> lk(m_PauseMutex);
        m_PauseCv.wait(lk, [=]{return !m_Pause;});
//        cerr << "requestPath unblocked (with the lock)" << endl;
        lk.unlock();
//        cerr << "requestPath released the lock" << endl;

        if (m_RibbonManager.done())
//        if (m_InternalsManager.finish())
        {
            this_thread::sleep_for(chrono::milliseconds(1000));
            cerr << "Finished path; pausing" << endl;
            pause();
            continue;
        }

        m_TrajectoryPublisher->displayRibbons(m_RibbonManager);

        if (m_MapMutex.try_lock()) {
            if (m_NewMap) {
                m_Planner->updateMap(m_NewMap);
            }
            m_NewMap = nullptr;
            m_MapMutex.unlock();
        }

        start = m_TrajectoryPublisher->getTime();
//        auto newlyCoveredList = m_InternalsManager.getNewlyCovered();
//        vector<pair<double, double>> newlyCovered;
//        for (auto p : newlyCoveredList) newlyCovered.emplace_back(p.x, p.y);
        vector<State> plan;
//        cerr << "ROS time is now " << m_TrajectoryPublisher->getTime() << endl;
        auto startState = m_TrajectoryPublisher->getEstimatedState(m_TrajectoryPublisher->getTime() + c_PlanningTimeSeconds);
        if (startState.time == -1) { // if the state estimator returns an error naively do it ourselves
            startState.setEstimate(m_TrajectoryPublisher->getTime() + c_PlanningTimeSeconds - m_LastState.time, m_LastState);
        }
//        cerr << "Calling planner with state " << startState.toString() << endl;
        try {
            // change this line to use controller's starting estimate
//            plan = m_Planner->plan(newlyCovered, m_InternalsManager.getStart(), DynamicObstaclesManager(), 0.95);
            // TODO! -- low-key race condition with the ribbon manager here but it might be fine
            // NOTE: changed the time remaining from 0.95 to 0.7 to hopefully allow the controller to update
            // its estimates of our trajectory
            plan = m_Planner->plan(m_RibbonManager, startState, DynamicObstaclesManager(),
                                   start + c_PlanningTimeSeconds - m_TrajectoryPublisher->getTime(),
                                   MaxSpeed, TurningRadius, CoverageMaxSpeed, CoverageTurningRadius);
        } catch (...) {
            cerr << "Exception thrown while planning; pausing" << endl;
            pause();
            throw;
        }

//        cerr << "Setting new path of length " << plan.size() << endl;
//        m_InternalsManager.setNewPath(plan);
        m_TrajectoryPublisher->publishTrajectory(plan);
        m_TrajectoryPublisher->displayTrajectory(plan, true);
        end = m_TrajectoryPublisher->getTime();
        sleeptime = (end - start <= c_PlanningTimeSeconds) ? ((int)((c_PlanningTimeSeconds - (end - start)) * 1000)) : 0;

        this_thread::sleep_for(chrono::milliseconds(sleeptime));

//        if (plannerIsDead()) pause();
    }
}

void Executive::addToCover(int x, int y)
{
    m_InternalsManager.addToCover(x, y);
}

void Executive::startPlanner(const string& mapFile, double latitude, double longitude)
{
    cerr << "Starting planner" << endl;

    shared_ptr<Map> map;
    try {
        map = make_shared<GeoTiffMap>(mapFile, longitude, latitude);
    }
    catch (...) {
        map = make_shared<Map>();
    }
//    m_Planner = std::unique_ptr<Planner>(new Planner(2.3, 8, Map()));
    m_Planner = std::unique_ptr<Planner>(new AStarPlanner(MaxSpeed, TurningRadius, map));

    // assume you've already set up path to cover
//    vector<pair<double, double>> toCover;
//    for (point p : m_InternalsManager.getCovered())
//        toCover.emplace_back(p.x, p.y);
//    m_Planner->addToCover(toCover);

    cerr << "Planner is up and running" << endl;

    // planner is running so the listener and updater threads should too
    unPause();
}

void Executive::startThreads()
{
    m_Running = true;
//    cerr << "Starting thread to publish to controller" << endl;
//    m_TrajectoryPublishingFuture = async(launch::async, &Executive::sendAction, this);
    cerr << "Starting thread to listen to planner" << endl;
    m_PlanningFuture = async(launch::async, &Executive::requestPath, this);
}

void Executive::terminate()
{
    if (!m_Running) return;
    m_Running = false;

    // un-pause so threads can terminate
    unPause();
}

void Executive::pause()
{
    m_PauseMutex.lock();
    if (m_Pause) {
        m_PauseMutex.unlock();
        return;
    }
    m_Pause = true;
    m_PauseMutex.unlock();

    if (!m_Running) return;

    // tell the node we achieved the goal
    m_TrajectoryPublisher->allDone();
}


bool Executive::plannerIsRunning() {
    return m_Running;
}

void Executive::unPause() {
    m_PauseMutex.lock();
    m_Pause = false;
    m_PauseMutex.unlock();
    m_PauseCv.notify_all();
}

void Executive::updateDynamicObstacle(uint32_t mmsi, State obstacle) {
//    m_InternalsManager.updateDynamicObstacle(mmsi, obstacle);
    m_DynamicObstaclesManager.update(mmsi, inventDistributions(obstacle));
}

void Executive::refreshMap(std::string pathToMapFile, double latitude, double longitude) {
    thread([this, pathToMapFile, latitude, longitude] {
        std::lock_guard<std::mutex> lock(m_MapMutex);
        if (m_CurrentMapPath != pathToMapFile) {
            // could take some time for I/O, Dijkstra on entire map
            try {
                // If the name looks like it's one of our gridworld maps, load it in that format, otherwise assume GeoTIFF
                if (pathToMapFile.find(".map") == -1) {
                    m_NewMap = make_shared<GeoTiffMap>(pathToMapFile, longitude, latitude);
                } else {
                    m_NewMap = make_shared<GridWorldMap>(pathToMapFile);
                }
                m_CurrentMapPath = pathToMapFile;
            }
            catch (...) {
                // swallow all errors in this thread
                cerr << "Encountered an error loading map at path " << pathToMapFile << endl;
                m_NewMap = nullptr;
                m_CurrentMapPath = "";
            }
        }
    }).detach();
}

void Executive::addRibbon(double x1, double y1, double x2, double y2) {
    m_RibbonManager.add(x1, y1, x2, y2);
}

std::vector<Distribution> Executive::inventDistributions(State obstacle) {
    std::vector<Distribution> distributions;
    double mean[2] = {obstacle.x, obstacle.y};
    double covariance[2][2] = {{0, 5},{5, 0}};
    distributions.emplace_back(mean, covariance, obstacle.heading, obstacle.time);
    obstacle.setEstimate(1, obstacle);
    mean[0] = obstacle.x; mean[1] = obstacle.y;
    distributions.emplace_back(mean, covariance, obstacle.heading, obstacle.time);
    return distributions;
}

void Executive::clearRibbons() {
    m_RibbonManager = RibbonManager();
}

void Executive::setVehicleConfiguration(double maxSpeed, double turningRadius, double coverageMaxSpeed,
                                        double coverageTurningRadius) {
    MaxSpeed = maxSpeed;
    TurningRadius = turningRadius;
    CoverageMaxSpeed = coverageMaxSpeed;
    CoverageTurningRadius = coverageTurningRadius;
}
