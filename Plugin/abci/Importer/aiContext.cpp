#include "pch.h"
#include "aiInternal.h"
#include "aiContext.h"
#include "aiObject.h"
#include <limits>


std::string ToString(const aiConfig &v)
{
    std::ostringstream oss;

    oss << "{swapHandedness: " << (v.swap_handedness ? "true" : "false");
    oss << ", swapFaceWinding: " << (v.swap_face_winding ? "true" : "false");
    oss << ", normalsMode: " << (v.normals_mode == aiNormalsMode::ReadFromFile
        ? "read_from_file"
        : (v.normals_mode == aiNormalsMode::ComputeIfMissing
            ? "compute_if_missing"
            : (v.normals_mode == aiNormalsMode::AlwaysCompute
                ? "always_compute"
                : "ignore")));
    oss << ", tangentsMode: " << (v.tangents_mode == aiTangentsMode::None
        ? "none"
        : (v.tangents_mode == aiTangentsMode::Compute
            ? "smooth"
            : "split"));
    oss << ", aspectRatio: " << v.aspect_ratio;

    return oss.str();
}

aiContextManager aiContextManager::s_instance;

aiContext* aiContextManager::getContext(int uid)
{
    auto it = s_instance.m_contexts.find(uid);
if (it != s_instance.m_contexts.end()) {
        DebugLog("Using already created context for gameObject with ID %d", uid);
        return it->second.get();
    }

    auto ctx = new aiContext(uid);
    s_instance.m_contexts[uid].reset(ctx);
    DebugLog("Register context for gameObject with ID %d", uid);
    return ctx;
}

void aiContextManager::destroyContext(int uid)
{
    auto it = s_instance.m_contexts.find(uid);
    if (it != s_instance.m_contexts.end()) {
        DebugLog("Unregister context for gameObject with ID %d", uid);
        s_instance.m_contexts.erase(it);
    }
}

void aiContextManager::destroyContextsWithPath(const char* assetPath)
{
    auto path = aiContext::normalizePath(assetPath);
    for (auto it = s_instance.m_contexts.begin(); it != s_instance.m_contexts.end(); ++it) {
        if (it->second->getPath() == path) {
            DebugLog("Unregister context for gameObject with ID %s", it->second->getPath().c_str());
            s_instance.m_contexts.erase(it);
        }
    }
}

aiContextManager::~aiContextManager()
{
    if (m_contexts.size()) {
        DebugWarning("%lu remaining context(s) registered", m_contexts.size());
    }

    m_contexts.clear();
}
// ---

aiContext::aiContext(int uid)
    : m_uid(uid)
{
}

aiContext::~aiContext()
{
    m_top_node.reset();
    m_archive.reset();
}

Abc::IArchive aiContext::getArchive() const
{
    return m_archive;
}

const std::string& aiContext::getPath() const
{
    return m_path;
}


int aiContext::getNumTimeSamplings()
{
    return (int)m_archive.getNumTimeSamplings();
}

void aiContext::getTimeSampling(int i, aiTimeSamplingData& dst)
{
    auto ts = m_archive.getTimeSampling(i);
    auto tst = ts->getTimeSamplingType();

    dst.numTimes = (int)ts->getNumStoredTimes();
    if (tst.isUniform() || tst.isCyclic()) {
        int numCycles = int(m_archive.getMaxNumSamplesForTimeSamplingIndex(i) / tst.getNumSamplesPerCycle());

        dst.type = tst.isUniform() ? aiTimeSamplingType::Uniform : aiTimeSamplingType::Cyclic;
        dst.interval = (float)tst.getTimePerCycle();
        dst.start_time = (float)ts->getStoredTimes()[0];
        dst.end_time = dst.start_time + dst.interval * (numCycles - 1);
        dst.numTimes = (int)ts->getNumStoredTimes();
        dst.times = const_cast<double*>(&ts->getStoredTimes()[0]);
    }
    else if (tst.isAcyclic()) {
        dst.type = aiTimeSamplingType::Acyclic;
        dst.start_time = (float)ts->getSampleTime(0);
        dst.end_time = (float)ts->getSampleTime(ts->getNumStoredTimes() - 1);
        dst.numTimes = (int)ts->getNumStoredTimes();
        dst.times = const_cast<double*>(&ts->getStoredTimes()[0]);
    }
}

void aiContext::copyTimeSampling(int i, aiTimeSamplingData& dst)
{
    int dst_numSamples = dst.numTimes;
    double *dst_samples = dst.times;

    getTimeSampling(i, dst);

    if (dst.type == aiTimeSamplingType::Acyclic) {
        const auto& times = m_archive.getTimeSampling(i)->getStoredTimes();
        if (dst_samples && dst_numSamples >= (int)times.size()) {
            // memcpy() is way faster than std::copy() on VC...
            memcpy(dst.times, &times[0], sizeof(times[0])*times.size());
        }
    }
}

int aiContext::getTimeSamplingIndex(Abc::TimeSamplingPtr ts)
{
    int n = m_archive.getNumTimeSamplings();
    for (int i = 0; i < n; ++i) {
        if (m_archive.getTimeSampling(i) == ts) {
            return i;
        }
    }
    return 0;
}


int aiContext::getUid() const
{
    return m_uid;
}

const aiConfig& aiContext::getConfig() const
{
    return m_config;
}

void aiContext::setConfig(const aiConfig &config)
{
    DebugLog("aiContext::setConfig: %s", ToString(config).c_str());
    m_config = config;
}

void aiContext::gatherNodesRecursive(aiObject *n)
{
    abcObject &abc = n->getAbcObject();
    size_t numChildren = abc.getNumChildren();
    
    for (size_t i = 0; i < numChildren; ++i)
    {
        aiObject *child = n->newChild(abc.getChild(i));
        gatherNodesRecursive(child);
    }
}

void aiContext::reset()
{
    DebugLog("aiContext::reset()");

    m_top_node.reset();
    
    m_path = "";
    m_archive.reset();

    m_timeRange[0] = 0.0f;
    m_timeRange[1] = 0.0f;
}

std::string aiContext::normalizePath(const char *inPath)
{
    std::string path;

    if (inPath != nullptr)
    {
        path = inPath;

        #ifdef _WIN32
        
        size_t n = path.length();
        
        for (size_t i=0; i<n; ++i)
        {
            char c = path[i];
            if (c == '\\')
            {
                path[i] = '/';
            }
            else if (c >= 'A' && c <= 'Z')
            {
                path[i] = 'a' + (c - 'A');
            }
        }
        
        #endif
    }

    return path;
}

bool aiContext::load(const char *inPath)
{
    std::string path = normalizePath(inPath);

    DebugLog("aiContext::load: '%s'", path.c_str());

    if (path == m_path && m_archive)
    {
        DebugLog("Context already loaded for gameObject with id %d", m_uid);
        return true;
    }

    DebugLog("Alembic file path changed from '%s' to '%s'. Reset context.", m_path.c_str(), path.c_str());
    aiLogger::Indent(1);

    reset();

    if (path.length() == 0)
    {
        aiLogger::Unindent(1);
        return false;
    }

    m_path = path;
    
    if (!m_archive.valid())
    {
        DebugLog("Archive '%s' not yet opened", inPath);

        try
        {
            DebugLog("Trying to open AbcCoreOgawa::ReadArchive...");
            m_archive = Abc::IArchive(AbcCoreOgawa::ReadArchive(std::thread::hardware_concurrency()), path);
        }
        catch (Alembic::Util::Exception e)
        {
            DebugLog("Failed (%s)", e.what());

            try
            {
                DebugLog("Trying to open AbcCoreHDF5::ReadArchive...");
                m_archive = Abc::IArchive(AbcCoreHDF5::ReadArchive(), path);
            }
            catch (Alembic::Util::Exception e2)
            {
                DebugLog("Failed (%s)", e2.what());
            }
        }
    }
    else
    {
        DebugLog("Archive '%s' already opened", inPath);
    }

    if (m_archive.valid())
    {
        abcObject abcTop = m_archive.getTop();
        m_top_node.reset(new aiObject(this, nullptr, abcTop));
        gatherNodesRecursive(m_top_node.get());

        m_timeRange[0] = std::numeric_limits<double>::max();
        m_timeRange[1] = -std::numeric_limits<double>::max();

        for (unsigned int i=0; i<m_archive.getNumTimeSamplings(); ++i)
        {
            AbcCoreAbstract::TimeSamplingPtr ts = m_archive.getTimeSampling(i);

            AbcCoreAbstract::TimeSamplingType tst = ts->getTimeSamplingType();

            // Note: alembic guaranties we have at least one stored time

            if (tst.isCyclic() || tst.isUniform())
            {
                int numCycles = int(m_archive.getMaxNumSamplesForTimeSamplingIndex(i) / tst.getNumSamplesPerCycle());

                m_timeRange[0] = ts->getStoredTimes()[0];
                m_timeRange[1] = m_timeRange[0] + (numCycles - 1) * tst.getTimePerCycle();

                if (numCycles > m_numFrames) m_numFrames = numCycles;
            }
            else if (tst.isAcyclic())
            {
                m_timeRange[0] = ts->getSampleTime(0);
                m_timeRange[1] = ts->getSampleTime(ts->getNumStoredTimes() - 1);

                if (ts->getNumStoredTimes() > m_numFrames) m_numFrames = ts->getNumStoredTimes();
            }
        }

        if (m_timeRange[0] > m_timeRange[1])
        {
            m_timeRange[0] = 0.0;
            m_timeRange[1] = 0.0;
        }

        DebugLog("Succeeded");

        aiLogger::Unindent(1);
        return true;
    }
    else
    {
        aiLogger::Error("Invalid archive '%s'", inPath);
        aiLogger::Unindent(1);
        reset();
        return false;
    }
}

float aiContext::getStartTime() const
{
    return float(m_timeRange[0]);
}

float aiContext::getEndTime() const
{
    return float(m_timeRange[1]);
}

int aiContext::getFrameCount() const
{
    return (int)m_numFrames;
}

aiObject* aiContext::getTopObject() const
{
    return m_top_node.get();
}

void aiContext::updateSamples(float time)
{
    DebugLog("aiContext::updateSamples()");
    const abcSampleSelector ss = aiTimeToSampleSelector(time);

    eachNodes([ss](aiObject *o) {
        o->updateSample(ss);
    });
}