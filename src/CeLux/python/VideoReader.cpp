#include "Python/VideoReader.hpp"
#include <pybind11/pybind11.h>

namespace py = pybind11;
VideoReader::VideoReader(const std::string& filePath, const std::string& device,
                         const std::string& dataType)
    : decoder(nullptr), currentIndex(0), start_frame(0), end_frame(-1),
      torchDevice(torch::kCPU)
{
    try
    {
        // Determine the backend enum from the device string
        celux::backend backend;
        if (device == "cuda")
        {
            if (!torch::cuda::is_available())
            {
                throw std::runtime_error("CUDA is not available. Please install a "
                                         "CUDA-enabled version of celux.");
            }
            if (torch::cuda::device_count() == 0)
            {
                throw std::runtime_error(
                    "No CUDA devices found. Please check your CUDA installation.");
            }
          
            torchDevice = torch::kCUDA;
            backend = celux::backend::CUDA;
            torchDevice = torch::Device(torch::kCUDA);
        }
        else if (device == "cpu")
        {
            torchDevice = torch::kCPU;
            backend = celux::backend::CPU;
            torchDevice = torch::Device(torch::kCPU);
        }
        else
        {
            throw std::invalid_argument("Unsupported device: " + device);
        }

        // Map dataType string to celux::dataType enum and torch::Dtype
        celux::dataType dtype;
        torch::Dtype torchDataType;

        if (dataType == "uint8")
        {
            torchDataType = torch::kUInt8;
            dtype = celux::dataType::UINT8;
        }
        else if (dataType == "float32")
        {
            torchDataType = torch::kFloat32;
            dtype = celux::dataType::FLOAT32;
        }
        else if (dataType == "float16")
        {
            torchDataType = torch::kFloat16;
            dtype = celux::dataType::FLOAT16;
        }
        else
        {
            throw std::invalid_argument("Unsupported dataType: " + dataType);
        }

        // Create the converter using the factory
        convert = celux::Factory::createConverter(
            backend, celux::ConversionType::NV12ToRGB, dtype);

        // Create the decoder using the factory
        decoder = celux::Factory::createDecoder(backend, filePath, std::move(convert));

        // Retrieve video properties
        properties = decoder->getVideoProperties();

        // Initialize tensors based on backend and data type
        if (backend == celux::backend::CUDA)
        {
            RGBTensor = torch::empty(
                {properties.height, properties.width, 3},
                torch::TensorOptions().dtype(torchDataType).device(torch::kCUDA));

            // For CUDA, cpuTensor is not used. You might want to remove it or keep it
            // for CPU operations. If keeping, initialize it on CPU.
            cpuTensor = torch::empty(
                {properties.height, properties.width, 3},
                torch::TensorOptions().dtype(torchDataType).device(torch::kCUDA));
        }
        else // CPU
        {
            // For CPU, initialize cpuTensor on CPU
            cpuTensor = torch::empty(
                {properties.height, properties.width, 3},
                torch::TensorOptions().dtype(torchDataType).device(torch::kCPU));

            // RGBTensor is not used on CPU. You might want to remove it or keep it for
            // GPU operations. If keeping, initialize it on CUDA.
            RGBTensor = torch::empty(
                {properties.height, properties.width, 3},
                torch::TensorOptions().dtype(torchDataType).device(torch::kCPU));
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in VideoReader constructor: " << ex.what() << std::endl;
        throw; // Re-throw exception after logging
    }
}

VideoReader::~VideoReader()
{
    close();
}

void VideoReader::setRange(int start, int end)
{
    // Handle negative indices by converting them to positive frame numbers
    if (start < 0)
        start = properties.totalFrames + start;
    if (end < 0)
        end = properties.totalFrames + end;

    // Validate the adjusted frame range
    if (start < 0 || end < 0)
    {
        throw std::runtime_error("Frame indices out of range after adjustment.");
    }
    if (end <= start)
    {
        throw std::runtime_error(
            "end_frame must be greater than start_frame after adjustment.");
    }

    // Make end_frame exclusive by subtracting one
    end = end - 1;

    start_frame = start;
    end_frame = end;
}

void VideoReader::close()
{
    if (convert)
    {
        convert->synchronize();
        convert.reset();
    }
    if (decoder)
    {
        decoder->close(); // Assuming Decoder has a close method
        decoder.reset();
    }
}

torch::Tensor VideoReader::readFrame()
{
    int result;

    // Release GIL during decoding
    {
        py::gil_scoped_release release;
        result = decoder->decodeNextFrame(RGBTensor.data_ptr());
    }

    if (result == 1) // Frame decoded successfully
    {
        // No need to acquire GIL for tensor operations if they don't interact with
        // Python

        // Acquire GIL before returning tensor to Pytho
        cpuTensor.copy_(RGBTensor, /*non_blocking=*/true);
        py::gil_scoped_acquire acquire;
        return cpuTensor;
    }
    else if (result == 0) // End of video stream
    {
        // Acquire GIL before throwing exception
        py::gil_scoped_acquire acquire;
        throw py::stop_iteration();
    }
    else // Decoding failed
    {
        // Acquire GIL before throwing exception
        py::gil_scoped_acquire acquire;
        throw std::runtime_error("Failed to decode the next frame.");
    }
}


bool VideoReader::seek(double timestamp)
{
    bool success;

    // Release GIL during seeking
    {
        py::gil_scoped_release release;
        success = decoder->seek(timestamp);
    }

    return success;
}

std::vector<std::string> VideoReader::supportedCodecs()
{
    return decoder->listSupportedDecoders();
}

py::dict VideoReader::getProperties() const
{
    py::dict props;
    props["width"] = properties.width;
    props["height"] = properties.height;
    props["fps"] = properties.fps;
    props["duration"] = properties.duration;
    props["total_frames"] = properties.totalFrames;
    props["pixel_format"] = av_get_pix_fmt_name(properties.pixelFormat);
    props["has_audio"] = properties.hasAudio;
    return props;
}

void VideoReader::reset()
{
    seek(0.0); // Reset to the beginning
    currentIndex = 0;
}

bool VideoReader::seekToFrame(int frame_number)
{
    if (frame_number < 0 || frame_number >= properties.totalFrames)
    {
        return false; // Out of range
    }

    // Convert frame number to timestamp in seconds
    double timestamp = static_cast<double>(frame_number) / properties.fps;

    return seek(timestamp);
}

VideoReader& VideoReader::iter()
{
    currentIndex = start_frame;
    seekToFrame(start_frame);
    return *this;
}

 torch::Tensor VideoReader::next()
{
    if (end_frame >= 0 && currentIndex > end_frame)
    {
        throw py::stop_iteration(); // Stop iteration if range is exhausted
    }

     torch::Tensor frame = readFrame();
    if (frame.numel() == 0)
    {
        throw py::stop_iteration(); // Stop iteration if no more frames are available
    }

    currentIndex++;
    return frame;
}

void VideoReader::enter()
{
    // Resources are already initialized in the constructor
}

void VideoReader::exit(const py::object& exc_type, const py::object& exc_value,
                       const py::object& traceback)
{
    close(); // Close the video reader and free resources
}

int VideoReader::length() const
{
    return properties.totalFrames;
}

void VideoReader::sync()
{
    // Release GIL during synchronization
    {
        py::gil_scoped_release release;
        if (convert)
        {
            convert->synchronize();
        }
    }
}
