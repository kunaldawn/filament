/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "cmgen.h"

#include "utilities.h"
#include "Cubemap.h"
#include "CubemapIBL.h"
#include "CubemapUtils.h"

#include <imageio/ImageDecoder.h>
#include <imageio/ImageEncoder.h>

#include <utils/Path.h>

#include <getopt/getopt.h>

#include <iostream>
#include <fstream>
#include <iomanip>

using namespace math;
using namespace image;

static void printUsage(char* name) {
    std::string exec_name(utils::Path(name).getName());
    std::string usage(
            "CMGEN is a command-line tool for generating SH and mipmap levels from an env map.\n"
            "Cubemaps and equirectangular formats are both supported, automatically detected \n"
            "according to the aspect ratio of the source image.\n"
            "\n"
            "Usages:\n"
            "    CMGEN [options] <input-file>\n"
            "    CMGEN [options] <uv[N]>\n"
            "\n"
            "Supported input formats:\n"
            "    PNG, 8 and 16 bits\n"
            "    Radiance (.hdr)\n"
            "    Photoshop (.psd), 16 and 32 bits\n"
            "    OpenEXR (.exr)\n"
            "\n"
            "Options:\n"
            "   --help, -h\n"
            "       print this message\n\n"
            "   --license\n"
            "       Print copyright and license information\n\n"
            "   --quiet, -q\n"
            "       Quiet mode. Suppress all non-error output\n\n"
            "   --format=[exr|hdr|psd|rgbm|png|dds], -f [exr|hdr|psd|rgbm|png|dds]\n"
            "       specify output file format\n\n"
            "   --compression=COMPRESSION, -c COMPRESSION\n"
            "       format specific compression:\n"
            "           PNG: Ignored\n"
            "           PNG RGBM: Ignored\n"
            "           Radiance: Ignored\n"
            "           Photoshop: 16 (default), 32\n"
            "           OpenEXR: RAW, RLE, ZIPS, ZIP, PIZ (default)\n"
            "           DDS: 8, 16 (default), 32\n\n"
            "   --size=power-of-two, -s power-of-two\n"
            "       size of the output cubemaps (base level), 256 by default\n\n"
            "   --deploy=dir, -x dir\n"
            "       Generate everything needed for deployment into <dir>\n\n"
            "   --extract=dir\n"
            "       Extract faces of the cubemap into <dir>\n\n"
            "   --extract-blur=roughness\n"
            "       Blurs the cubemap before saving the faces using the roughness blur\n\n"
            "   --mirror\n"
            "       Mirrors generated cubemaps for reflections\n\n"
            "   --ibl-samples=numSamples\n"
            "       Number of samples to use for IBL integrations (default 1024)\n\n"
            "\n"
            "Private use only:\n"
            "   --ibl-dfg=filename.[exr|hdr|psd|png|rgbm|dds|h|hpp|c|cpp|inc|txt]\n"
            "       Computes the IBL DFG LUT\n\n"
            "   --ibl-dfg-multiscatter\n"
            "       If --ibl-dfg is set, computes the DFG for multi-scattering GGX\n\n"
            "   --ibl-is-mipmap=dir\n"
            "       Generates mipmap for pre-filtered importance sampling\n\n"
            "   --ibl-ld=dir\n"
            "       Roughness prefilter into <dir>\n\n"
            "   --sh=bands\n"
            "       SH decomposition of input cubemap\n\n"
            "   --sh-output=filename.[exr|hdr|psd|rgbm|png|dds|txt]\n"
            "       SH output format. The filename extension determines the output format\n\n"
            "   --sh-irradiance, -i\n"
            "       Irradiance SH coefficients\n\n"
            "   --sh-shader\n"
            "       Generate irradiance SH for shader code\n\n"
            "   --debug, -d\n"
            "       Generate extra data for debugging\n\n"
    );
    const std::string from("CMGEN");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
         usage.replace(pos, from.length(), exec_name);
    }
    printf("%s", usage.c_str());
}

static void license() {
    std::cout <<
    #include "licenses/licenses.inc"
    ;
}

static int handleCommandLineArgments(int argc, char* argv[]) {
    static constexpr const char* OPTSTR = "hqidf:c:s:x:";
    static const struct option OPTIONS[] = {
            { "help",                       no_argument, nullptr, 'h' },
            { "license",                    no_argument, nullptr, 'l' },
            { "quiet",                      no_argument, nullptr, 'q' },
            { "format",               required_argument, nullptr, 'f' },
            { "compression",          required_argument, nullptr, 'c' },
            { "size",                 required_argument, nullptr, 's' },
            { "extract",              required_argument, nullptr, 'e' },
            { "extract-blur",         required_argument, nullptr, 'r' },
            { "sh",                   optional_argument, nullptr, 'z' },
            { "sh-output",            required_argument, nullptr, 'o' },
            { "sh-irradiance",              no_argument, nullptr, 'i' },
            { "sh-shader",                  no_argument, nullptr, 'b' },
            { "ibl-is-mipmap",        required_argument, nullptr, 'y' },
            { "ibl-ld",               required_argument, nullptr, 'p' },
            { "ibl-dfg",              required_argument, nullptr, 'a' },
            { "ibl-dfg-multiscatter",       no_argument, nullptr, 'u' },
            { "ibl-samples",          required_argument, nullptr, 'k' },
            { "deploy",               required_argument, nullptr, 'x' },
            { "mirror",                     no_argument, nullptr, 'm' },
            { "debug",                      no_argument, nullptr, 'd' },
            { nullptr, 0, 0, 0 }  // termination of the option list
    };
    int opt;
    int option_index = 0;
    int num_sh_bands = 3;
    bool format_specified = false;
    while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(optarg ? optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
                break;
            case 'l':
                license();
                exit(0);
                break;
            case 'q':
                g_quiet = true;
                break;
            case 'f':
                if (arg == "png") {
                    g_format = ImageEncoder::Format::PNG;
                    format_specified = true;
                }
                if (arg == "hdr") {
                    g_format = ImageEncoder::Format::HDR;
                    format_specified = true;
                }
                if (arg == "rgbm") {
                    g_format = ImageEncoder::Format::RGBM;
                    format_specified = true;
                }
                if (arg == "exr") {
                    g_format = ImageEncoder::Format::EXR;
                    format_specified = true;
                }
                if (arg == "psd") {
                    g_format = ImageEncoder::Format::PSD;
                    format_specified = true;
                }
                if (arg == "dds") {
                    g_format = ImageEncoder::Format::DDS_LINEAR;
                    format_specified = true;
                }
                break;
            case 'c':
                g_compression = arg;
                break;
            case 's':
                g_output_size = std::stoul(arg);
                if (!isPOT(g_output_size)) {
                    std::cerr << "output size must be a power of two" << std::endl;
                    exit(0);
                }
                break;
            case 'z':
                g_sh_compute = 1;
                g_sh_output = true;
                try {
                    num_sh_bands = std::stoi(arg);
                } catch (std::invalid_argument &e) {
                    // keep default value
                }
                break;
            case 'o':
                g_sh_compute = 1;
                g_sh_output = true;
                g_sh_file = ShFile::SH_CROSS;
                g_sh_filename = arg;
                if (g_sh_filename.getExtension() == "txt") {
                    g_sh_file = ShFile::SH_TEXT;
                }
                break;
            case 'i':
                g_sh_compute = 1;
                g_sh_irradiance = true;
                break;
            case 'b':
                g_sh_compute = 1;
                g_sh_irradiance = true;
                g_sh_shader = true;
                break;
            case 'e':
                g_extract_dir = arg;
                g_extract_faces = true;
                break;
            case 'r':
                g_extract_blur = std::stod(arg);
                if (g_extract_blur < 0 || g_extract_blur > 1) {
                    std::cerr << "roughness (blur) parameter must be between 0.0 and 1.0" <<
                    std::endl;
                    exit(0);
                }
                break;
            case 'y':
                g_is_mipmap = true;
                g_is_mipmap_dir = arg;
                break;
            case 'p':
                g_prefilter = true;
                g_prefilter_dir = arg;
                break;
            case 'a':
                g_dfg = true;
                g_dfg_filename = arg;
                break;
            case 'u':
                g_dfg_multiscatter = true;
                break;
            case 'k':
                g_num_samples = (size_t)std::stoi(arg);
                break;
            case 'x':
                g_deploy = true;
                g_deploy_dir = arg;
                break;
            case 'd':
                g_debug = true;
                break;
            case 'm':
                g_mirror = true;
                break;
        }
    }

    if (g_deploy && !format_specified) {
        g_format = ImageEncoder::Format::RGBM;
    }

    if (num_sh_bands && g_sh_compute) {
        g_sh_compute = (size_t) num_sh_bands;
    }
    return optind;
}

int main(int argc, char* argv[]) {
    int option_index = handleCommandLineArgments(argc, argv);
    int num_args = argc - option_index;
    if (!g_dfg && num_args < 1) {
        printUsage(argv[0]);
        return 1;
    }

    if (g_dfg) {
        if (!g_quiet) {
            std::cout << "Generating IBL DFG LUT..." << std::endl;
        }
        size_t size = g_output_size ? g_output_size : 128;
        iblLutDfg(g_dfg_filename, size, g_dfg_multiscatter);
        if (num_args < 1) return 0;
    }

    std::string command(argv[option_index]);
    utils::Path iname(command);

    if (g_deploy) {
        utils::Path out_dir = g_deploy_dir + iname.getNameWithoutExtension();

        // generate pre-scaled irradiance sh to text file
        g_sh_compute = 3;
        g_sh_shader = true;
        g_sh_irradiance = true;
        g_sh_filename = out_dir + "sh.txt";
        g_sh_file = ShFile::SH_TEXT;
        g_sh_output = true;

        // faces
        g_extract_dir = g_deploy_dir;
        g_extract_faces = true;

        // prefilter
        g_prefilter = true;
        g_prefilter_dir = g_deploy_dir;
    }

    if (g_debug) {
        if (g_prefilter && !g_is_mipmap) {
            g_is_mipmap = true;
            g_is_mipmap_dir = g_prefilter_dir;
        }
    }

    // Images store the actual data
    std::vector<Image> images;

    // Cubemaps are just views on Images
    std::vector<Cubemap> levels;

    if (iname.exists()) {
        if (!g_quiet) {
            std::cout << "Decoding image..." << std::endl;
        }
        std::ifstream input_stream(iname.getPath(), std::ios::binary);
        Image inputImage = ImageDecoder::decode(input_stream, iname.getPath());
        if (!inputImage.isValid()) {
            std::cerr << "Unsupported image format!" << std::endl;
            exit(0);
        }
        if (inputImage.getChannelsCount() != 3) {
            std::cerr << "Input image must be RGB (3 channels)! This image has "
                      << inputImage.getChannelsCount() << " channels." << std::endl;
            exit(0);
        }

        CubemapUtils::clamp(inputImage);

        size_t width = inputImage.getWidth();
        size_t height = inputImage.getHeight();

        if ((isPOT(width) && (width * 3 == height * 4)) ||
            (isPOT(height) && (height * 3 == width * 4))) {
            // This is cross cubemap
            const bool isHorizontal = width > height;
            size_t dim = std::max(height, width) / 4;
            if (!g_quiet) {
                std::cout << "Loading cross... " << std::endl;
            }

            Image temp;
            Cubemap cml = CubemapUtils::create(temp, dim, isHorizontal);
            CubemapUtils::copyImage(temp, inputImage);
            cml.makeSeamless();
            images.push_back(std::move(temp));
            levels.push_back(std::move(cml));
        } else if (width == 2 * height) {
            // we assume a spherical (equirectangular) image, which we will convert to a cross image
            size_t dim = g_output_size ? g_output_size : 256;
            if (!g_quiet) {
                std::cout << "Converting equirectangular image... " << std::endl;
            }
            Image temp;
            Cubemap cml = CubemapUtils::create(temp, dim);
            CubemapUtils::equirectangularToCubemap(cml, inputImage);
            cml.makeSeamless();
            images.push_back(std::move(temp));
            levels.push_back(std::move(cml));
        } else {
            std::cerr << "Aspect ratio not supported: " << width << "x" << height << std::endl;
            std::cerr << "Supported aspect ratios:" << std::endl;
            std::cerr << "  2:1, lat/long or equirectangular" << std::endl;
            std::cerr << "  3:4, vertical cross (height must be power of two)" << std::endl;
            std::cerr << "  4:3, horizontal cross (width must be power of two)" << std::endl;
            exit(0);
        }
    } else {
        if (!g_quiet) {
            std::cout << "Generating image..." << std::endl;
        }

        size_t dim = g_output_size ? g_output_size : 256;
        Image temp;
        Cubemap cml = CubemapUtils::create(temp, dim);

        unsigned int p = 0;
        std::string name = iname.getNameWithoutExtension();
        if (sscanf(name.c_str(), "uv%u", &p) == 1) {
            CubemapUtils::generateUVGrid(cml, p);
        } else if (sscanf(name.c_str(), "brdf%u", &p) == 1) {
            double linear_roughness = sq(p / std::log2(dim));
            CubemapIBL::brdf(cml, linear_roughness);
        } else {
            CubemapUtils::generateUVGrid(cml, 1);
        }

        cml.makeSeamless();
        images.push_back(std::move(temp));
        levels.push_back(std::move(cml));
    }

    // Now generate all the mipmap levels
    generateMipmaps(levels, images);

    if (g_mirror) {
        if (!g_quiet) {
            std::cout << "Mirroring..." << std::endl;
        }

        std::vector<Cubemap> mirrorLevels;
        std::vector<Image> mirrorImages;

        for (auto& level : levels) {
            Image temp;
            Cubemap cml = CubemapUtils::create(temp, level.getDimensions());
            CubemapUtils::mirrorCubemap(cml, level);
            cml.makeSeamless();

            mirrorImages.push_back(std::move(temp));
            mirrorLevels.push_back(std::move(cml));
        }

        std::swap(levels, mirrorLevels);
        std::swap(images, mirrorImages);
    }

    if (g_sh_compute) {
        if (!g_quiet) {
            std::cout << "Spherical harmonics..." << std::endl;
        }
        Cubemap const& cm(levels[0]);
        sphericalHarmonics(iname, cm);
    }

    if (g_is_mipmap) {
        if (!g_quiet) {
            std::cout << "IBL mipmaps for prefiltered importance sampling..." << std::endl;
        }
        iblMipmapPrefilter(iname, images, levels, g_is_mipmap_dir);
    }

    if (g_prefilter) {
        if (!g_quiet) {
            std::cout << "IBL prefiltering..." << std::endl;
        }
        iblRoughnessPrefilter(iname, levels, g_prefilter_dir);
    }

    if (g_extract_faces) {
        Cubemap const& cm(levels[0]);
        if (g_extract_blur != 0) {
            if (!g_quiet) {
                std::cout << "Blurring..." << std::endl;
            }
            const double linear_roughness = g_extract_blur*g_extract_blur;
            const size_t dim = g_output_size ? g_output_size : cm.getDimensions();
            Image image;
            Cubemap blurred = CubemapUtils::create(image, dim);
            CubemapIBL::roughnessFilter(blurred, levels, linear_roughness, g_num_samples);
            if (!g_quiet) {
                std::cout << "Extract faces..." << std::endl;
            }
            extractCubemapFaces(iname, blurred, g_extract_dir);
        } else {
            if (!g_quiet) {
                std::cout << "Extract faces..." << std::endl;
            }
            extractCubemapFaces(iname, cm, g_extract_dir);
        }
    }

    return 0;
}