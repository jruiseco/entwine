/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/tree/multi-batcher.hpp>

#include <thread>

#include <pdal/PipelineManager.hpp>
#include <pdal/PointBuffer.hpp>
#include <pdal/Stage.hpp>
#include <pdal/StageFactory.hpp>

#include <entwine/third/json/json.h>
#include <entwine/tree/sleepy-tree.hpp>

namespace entwine
{

MultiBatcher::MultiBatcher(
        const S3Info& s3Info,
        const std::size_t numBatches,
        SleepyTree& sleepyTree,
        std::size_t snapshot)
    : m_s3(
            s3Info.awsAccessKeyId,
            s3Info.awsSecretAccessKey,
            s3Info.baseAwsUrl,
            s3Info.bucketName)
    , m_batches(numBatches)
    , m_available(numBatches)
    , m_originList()
    , m_sleepyTree(sleepyTree)
    , m_snapshot(snapshot)
    , m_allowAdd(true)
    , m_mutex()
    , m_cv()
{
    for (std::size_t i(0); i < m_available.size(); ++i)
    {
        m_available[i] = i;
    }
}

MultiBatcher::~MultiBatcher()
{
    gather();
}

void MultiBatcher::add(const std::string& filename)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]()->bool {
        return m_available.size() && m_allowAdd;
    });
    const std::size_t index(m_available.back());
    m_available.pop_back();

    Origin origin(m_originList.size());
    m_originList.push_back(filename);

    std::cout << "Adding " << filename << std::endl;
    lock.unlock();

    m_batches[index] = std::thread([this, index, &filename, origin]() {
        try
        {
            std::unique_ptr<pdal::PipelineManager> pipelineManager(
                    new pdal::PipelineManager());
            std::unique_ptr<pdal::StageFactory> stageFactory(
                    new pdal::StageFactory());
            std::unique_ptr<pdal::Options> readerOptions(
                    new pdal::Options());

            const std::string driver(stageFactory->inferReaderDriver(filename));
            if (driver.size())
            {
                const std::string localPath("./tmp/" + std::to_string(origin));

                {
                    // Retrieve remote file.
                    HttpResponse res(m_s3.get(filename));

                    // TODO Retry a few times.
                    if (res.code() != 200)
                    {
                        std::cout << "Couldn't fetch " + filename <<
                                " - Got: " << res.code() << std::endl;
                        throw std::runtime_error("Couldn't fetch " + filename);
                    }

                    std::shared_ptr<std::vector<uint8_t>> fileData(res.data());
                    std::ofstream writer(
                            localPath,
                            std::ofstream::binary |
                                std::ofstream::out |
                                std::ofstream::trunc);
                    writer.write(
                            reinterpret_cast<const char*>(fileData->data()),
                            fileData->size());
                    writer.close();
                }

                pipelineManager->addReader(driver);

                pdal::Stage* reader(static_cast<pdal::Reader*>(
                        pipelineManager->getStage()));
                readerOptions->add(pdal::Option("filename", localPath));
                reader->setOptions(*readerOptions.get());

                /*
                reader->setSpatialReference(
                        pdal::SpatialReference("EPSG:26915"));

                // Reproject to Web Mercator.
                pipelineManager->addFilter(
                        "filters.reprojection",
                        pipelineManager->getStage());

                pdal::Options srsOptions;
                srsOptions.add(
                        pdal::Option(
                            "in_srs",
                            pdal::SpatialReference("EPSG:26915")));
                srsOptions.add(
                        pdal::Option(
                            "out_srs",
                            pdal::SpatialReference("EPSG:3857")));

                pipelineManager->getStage()->setOptions(srsOptions);
                */

                pipelineManager->execute();
                const pdal::PointBufferSet& pbSet(pipelineManager->buffers());

                if (remove(localPath.c_str()) != 0)
                {
                    std::cout << "Couldn't delete " << localPath << std::endl;
                    throw std::runtime_error("Couldn't delete tmp file");
                }

                for (const auto pointBuffer : pbSet)
                {
                    m_sleepyTree.insert(pointBuffer.get(), origin);
                }
            }
            else
            {
                std::cout << "No driver found - " << filename << std::endl;
            }
        }
        catch (std::runtime_error& e)
        {
            std::cout << "Caught: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cout << "Exception in multi-batcher " << filename << std::endl;
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        m_available.push_back(index);
        m_batches[index].detach();
        lock.unlock();
        m_cv.notify_all();
    });

    if (m_snapshot && (origin + 1) % m_snapshot == 0)
    {
        takeSnapshot();
    }
}

void MultiBatcher::gather()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]()->bool {
        return m_available.size() == m_batches.size();
    });
}

void MultiBatcher::takeSnapshot()
{
    m_mutex.lock();
    m_allowAdd = false;
    m_mutex.unlock();

    try
    {
        std::cout << "Writing snapshot at " << m_originList.size() << std::endl;
        gather();

        std::ofstream dataStream;
        dataStream.open(
                m_sleepyTree.path() + "/manifest",
                std::ofstream::out | std::ofstream::trunc);
        Json::Value jsonManifest;
        jsonManifest["manifest"].resize(m_originList.size());
        for (std::size_t i(0); i < m_originList.size(); ++i)
        {
            jsonManifest["manifest"][static_cast<Json::ArrayIndex>(i)] =
                m_originList[i];
        }
        const std::string manifestString(jsonManifest.toStyledString());
        dataStream.write(manifestString.data(), manifestString.size());
        dataStream.close();

        m_sleepyTree.save();
    }
    catch (...)
    {
        std::cout << "Caught exception during snapshot" << std::endl;
    }

    m_mutex.lock();
    m_allowAdd = true;
    m_mutex.unlock();
    m_cv.notify_all();
}

} // namespace entwine
