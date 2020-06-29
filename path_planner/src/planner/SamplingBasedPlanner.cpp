#include "SamplingBasedPlanner.h"
#include <algorithm>
#include <utility>

SamplingBasedPlanner::SamplingBasedPlanner() {}

void SamplingBasedPlanner::pushVertexQueue(Vertex::SharedPtr vertex) {
    if (!vertex->isRoot() && vertex->parentEdge()->infeasible()) return;
    vertex->approxToGo(); // make sure it is calculated
    // prune vertices worse than the incumbent solution
    if (m_BestVertex && m_BestVertex->f() < vertex->f()) return; // assumes heuristic is admissible and consistent
    // make sure this isn't a goal with equal f to the incumbent
    if (m_BestVertex && m_BestVertex->f() == vertex->f() && goalCondition(vertex)) return;
    m_VertexQueue.push_back(vertex);
    std::push_heap(m_VertexQueue.begin(), m_VertexQueue.end(), getVertexComparator());
//    std::cerr << "Pushing to vertex queue: " << vertex->toString() << std::endl;
    visualizeVertex(vertex, "vertex", false);
}

std::shared_ptr<Vertex> SamplingBasedPlanner::popVertexQueue() {
    if (m_VertexQueue.empty()) throw std::out_of_range("Trying to pop an empty vertex queue");
    std::pop_heap(m_VertexQueue.begin(), m_VertexQueue.end(), getVertexComparator());
    auto ret = m_VertexQueue.back();
    m_VertexQueue.pop_back();
    return ret;
}

std::function<bool(std::shared_ptr<Vertex> v1,
                   std::shared_ptr<Vertex> v2)> SamplingBasedPlanner::getVertexComparator() {
    return [](const std::shared_ptr<Vertex>& v1, const std::shared_ptr<Vertex>& v2){
        return v1->getDepth() < v2->getDepth();
    };
}

std::function<bool(const State& s1, const State& s2)> SamplingBasedPlanner::getStateComparator(const State& origin) {
    return [&](const State& s1, const State& s2) {
        return s1.distanceTo(origin) > s2.distanceTo(origin);
    };
}

bool SamplingBasedPlanner::goalCondition(const std::shared_ptr<Vertex>& vertex) {
    return vertex->state().time() >= m_StartStateTime + m_Config.timeHorizon() ||
           (vertex->done() && vertex->state().time() >= m_StartStateTime + m_Config.timeMinimum());
}

void SamplingBasedPlanner::expand(const std::shared_ptr<Vertex>& sourceVertex, const DynamicObstaclesManager& obstacles) {
    
//    std::cerr << "Expanding vertex " << sourceVertex->toString() << std::endl;
    visualizeVertex(sourceVertex, "vertex", true);
    // add nearest point to cover
    if (!sourceVertex->done()) {
        auto s = sourceVertex->getNearestPointAsState();
        // TODO! -- get some set of near points
        if (sourceVertex->state().distanceTo(s) > m_Config.collisionCheckingIncrement()) {
            s.speed() = m_Config.maxSpeed();
            auto destinationVertex = Vertex::connect(sourceVertex, s, m_Config.turningRadius(), false);
            destinationVertex->parentEdge()->computeTrueCost(m_Config);
            pushVertexQueue(destinationVertex);
            // add again for coverage (may not be necessary, as we're unlikely to be covering stuff on the way to the nearest endpoint)
            destinationVertex = Vertex::connect(sourceVertex, s, m_Config.coverageTurningRadius(), true);
            destinationVertex->parentEdge()->computeTrueCost(m_Config);
            pushVertexQueue(destinationVertex);
        }
    }
    auto comp = getStateComparator(sourceVertex->state());
    auto dubinsComp = getDubinsComparator(sourceVertex->state());
    // heapify first by Euclidean distance
    std::make_heap(m_Samples.begin(), m_Samples.end(), comp);
    // Use another heap to sort by Dubins distance, skipping the samples which are farther away this time.
    // Making all the vertices adds some allocation overhead but it lets us cache the dubins paths
    std::vector<std::shared_ptr<Vertex>> bestSamples, bestCoverageSamples;
    bool regularDone = false, coverageDone = false;
    if (m_Config.coverageTurningRadius() <= 0) coverageDone = true;
    for (uint64_t i = 0; i < m_Samples.size() && (!regularDone || !coverageDone); i++) {
        auto sample = m_Samples.front();
        std::pop_heap(m_Samples.begin(), m_Samples.end() - i, comp);
        if (!regularDone && (bestSamples.size() < k() ||
            bestSamples.front()->parentEdge()->approxCost() > sample.distanceTo(sourceVertex->state()))) {
            if (sourceVertex->state().distanceTo(sample) > m_Config.collisionCheckingIncrement()) {
                // don't force speed to be anything in particular, allowing samples to come with unique speeds
                bestSamples.push_back(Vertex::connect(sourceVertex, sample, m_Config.turningRadius(), false));
                bestSamples.back()->parentEdge()->computeApproxCost();
                std::push_heap(bestSamples.begin(), bestSamples.end(), dubinsComp);
                if (bestSamples.size() > k()) {
                    std::pop_heap(bestSamples.begin(), bestSamples.end(), dubinsComp);
                    bestSamples.pop_back();
                }
            }
        } else {
            regularDone = true;
        }
        if (!coverageDone && (bestCoverageSamples.size() < k() ||
            bestCoverageSamples.front()->parentEdge()->approxCost() > sample.distanceTo(sourceVertex->state()))) {
            if (sourceVertex->state().distanceTo(sample) > m_Config.collisionCheckingIncrement()) {
                bestCoverageSamples.push_back(Vertex::connect(sourceVertex, sample, m_Config.coverageTurningRadius(), true));
                bestCoverageSamples.back()->parentEdge()->computeApproxCost();
                std::push_heap(bestCoverageSamples.begin(), bestCoverageSamples.end(), dubinsComp);
                if (bestCoverageSamples.size() > k()) {
                    std::pop_heap(bestCoverageSamples.begin(), bestCoverageSamples.end(), dubinsComp);
                    bestCoverageSamples.pop_back();
                }
            }
        } else {
            coverageDone = true;
        }
    }
    // Push the closest K onto the open list
    for (int i = 0; i < k(); i++) {
        if (i >= bestSamples.size()) break;
        auto destinationVertex = bestSamples.front();
        std::pop_heap(bestSamples.begin(), bestSamples.end() - i, dubinsComp);
        destinationVertex->parentEdge()->computeTrueCost(m_Config);
        pushVertexQueue(destinationVertex);
    }
    // and again for coverage edges
    for (int i = 0; i < k(); i++) {
        if (i >= bestCoverageSamples.size()) break;
        auto destinationVertex = bestCoverageSamples.front();
        std::pop_heap(bestCoverageSamples.begin(), bestCoverageSamples.end() - i, dubinsComp);
        destinationVertex->parentEdge()->computeTrueCost(m_Config);
        pushVertexQueue(destinationVertex);
    }
    m_ExpandedCount++;
}

int SamplingBasedPlanner::k() const {
    return m_Config.branchingFactor();
}

void SamplingBasedPlanner::addSamples(StateGenerator& generator, int n) {
    for (int i = 0; i < n; i++) {
        const auto s = generator.generate();
        m_Samples.push_back(s);
    }
}

void SamplingBasedPlanner::addSamples(StateGenerator& generator) {
    addSamples(generator, m_Samples.size());
}

void SamplingBasedPlanner::clearVertexQueue() {
    m_VertexQueue.clear();
}

std::function<bool(const std::shared_ptr<Vertex>& v1, const std::shared_ptr<Vertex>& v2)> SamplingBasedPlanner::getDubinsComparator(
        const State& origin) {
    return [&] (const std::shared_ptr<Vertex>& v1, const std::shared_ptr<Vertex>& v2) {
        // backwards from other state comparator because we want a max heap not a min heap
        return v1->parentEdge()->approxCost() < v2->parentEdge()->approxCost();
    };
}

DubinsPlan SamplingBasedPlanner::plan(const RibbonManager&, const State& start, PlannerConfig config,
                                      const DubinsPlan& previousPlan,
                                      double timeRemaining) {
    m_Config = config;
    m_StartStateTime = start.time();
    m_Samples.clear();
    m_VertexQueue.clear();
    double minX, maxX, minY, maxY, minSpeed = m_Config.maxSpeed(), maxSpeed = m_Config.maxSpeed();
    double magnitude = m_Config.maxSpeed() * m_Config.timeHorizon();
    minX = start.x() - magnitude;
    maxX = start.x() + magnitude;
    minY = start.y() - magnitude;
    maxY = start.y() + magnitude;

    StateGenerator generator = StateGenerator(minX, maxX, minY, maxY, minSpeed, maxSpeed, 7, m_RibbonManager); // lucky seed
    addSamples(generator, 1000);
    std::shared_ptr<Vertex> vertex;
    for (vertex = Vertex::makeRoot(start, m_RibbonManager);
         !goalCondition(vertex); vertex = popVertexQueue()) {
        expand(vertex, m_Config.obstaclesManager());
    }
    return tracePlan(vertex, false, m_Config.obstaclesManager());
}

void SamplingBasedPlanner::visualizeVertex(Vertex::SharedPtr v, const std::string& tag, bool expanded) {
    if (m_Config.visualizations()) {
        m_Config.visualizationStream() << (expanded? "Expanded " : "Generated ") <<
        v->toString() << " " << tag << " " << v->getPointerTreeString() << std::endl;
    }
}

bool SamplingBasedPlanner::vertexQueueEmpty() const {
    return m_VertexQueue.empty();
}

void SamplingBasedPlanner::visualizeRibbons(const RibbonManager& ribbonManager) {
    if (m_Config.visualizations()) {
        m_Config.visualizationStream() << ribbonManager.dumpRibbons() << "\nEnd Ribbons" << std::endl;
    }
}

void SamplingBasedPlanner::visualizePlan(const DubinsPlan& plan) {
    if (m_Config.visualizations()) {
        State s;
        s.time() = plan.getStartTime();
        while (s.time() < plan.getEndTime()) {
            plan.sample(s);
            m_Config.visualizationStream() << "State: (" << s.toStringRad() << "), f: " << 0 << ", g: " << 0 << ", h: " <<
                                           0 << " plan" << std::endl;
            s.time() += 1;
        }
    }
}


