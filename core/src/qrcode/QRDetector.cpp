/*
* Copyright 2016 Nu-book Inc.
* Copyright 2016 ZXing authors
* Copyright 2020 Axel Waggershauser
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

#include "QRDetector.h"
#include "QRVersion.h"
#include "BitMatrix.h"
#include "BitMatrixCursor.h"
#include "DetectorResult.h"
#include "PerspectiveTransform.h"
#include "RegressionLine.h"
#include "ConcentricFinder.h"
#include "GridSampler.h"
#include "ZXNumeric.h"
#include "LogMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

namespace ZXing {
namespace QRCode {

static auto FindFinderPatterns(const BitMatrix& image, bool tryHarder)
{
	constexpr int MIN_SKIP    = 3;           // 1 pixel/module times 3 modules/center
	constexpr int MAX_MODULES = 20 * 4 + 17; // support up to version 20 for mobile clients
	constexpr auto PATTERN    = FixedPattern<5, 7>{1, 1, 3, 1, 1};

	// Let's assume that the maximum version QR Code we support takes up 1/4 the height of the
	// image, and then account for the center being 3 modules in size. This gives the smallest
	// number of pixels the center could be, so skip this often. When trying harder, look for all
	// QR versions regardless of how dense they are.
	int height = image.height();
	int skip = (3 * height) / (4 * MAX_MODULES);
	if (skip < MIN_SKIP || tryHarder)
		skip = MIN_SKIP;

	std::vector<ConcentricPattern> res;

	for (int y = skip - 1; y < height; y += skip) {
		PatternRow row;
		image.getPatternRow(y, row);
		PatternView next = row;

		while (next = FindLeftGuard(next, 0, PATTERN, 0.5), next.isValid()) {
			PointF p(next.pixelsInFront() + next[0] + next[1] + next[2] / 2.0, y + 0.5);

			// make sure p is not 'inside' an already found pattern area
			if (FindIf(res, [p](const auto& old) { return distance(p, old) < old.size / 2; }) == res.end()) {
				auto pattern = LocateConcentricPattern(image, PATTERN, p,
													   Reduce(next) * 3 / 2); // 1.5 for very skewed samples
				if (pattern) {
					log(*pattern, 3);
					res.push_back(*pattern);
				}
			}

			next.skipPair();
			next.skipPair();
			next.extend();
		}
	}

	return res;
}

struct FinderPatternSet
{
	ConcentricPattern bl, tl, tr;
};

using FinderPatternSets = std::vector<FinderPatternSet>;

/**
 * @brief GenerateFinderPatternSets
 * @param patterns list of ConcentricPattern objects, i.e. found finder pattern squares
 * @return list of plausible finder pattern sets, sorted by decreasing plausibility
 */
static FinderPatternSets GenerateFinderPatternSets(std::vector<ConcentricPattern>&& patterns)
{
	std::sort(patterns.begin(), patterns.end(), [](const auto& a, const auto& b) { return a.size < b.size; });

	auto sets            = std::multimap<double, FinderPatternSet>();
	auto squaredDistance = [](PointF a, PointF b) { return dot((a - b), (a - b)); };

	int nbPatterns = Size(patterns);
	for (int i = 0; i < nbPatterns - 2; i++) {
		for (int j = i + 1; j < nbPatterns - 1; j++) {
			for (int k = j + 1; k < nbPatterns - 0; k++) {
				const auto* a = &patterns[i];
				const auto* b = &patterns[j];
				const auto* c = &patterns[k];
				// if the pattern sizes are too different to be part of the same symbol, skip this
				// and the rest of the innermost loop (sorted list)
				if (c->size > a->size * 2)
					break;

				// Orders the three points in an order [A,B,C] such that AB is less than AC
				// and BC is less than AC, and the angle between BC and BA is less than 180 degrees.

				auto distAB = squaredDistance(*a, *b);
				auto distBC = squaredDistance(*b, *c);
				auto distAC = squaredDistance(*a, *c);

				if (distBC >= distAB && distBC >= distAC) {
					std::swap(a, b);
					std::swap(distBC, distAC);
				} else if (distAB >= distAC && distAB >= distBC) {
					std::swap(b, c);
					std::swap(distAB, distAC);
				}

				// a^2 + b^2 = c^2 (Pythagorean theorem), and a = b (isosceles triangle).
				// Since any right triangle satisfies the formula c^2 - b^2 - a^2 = 0,
				// we need to check both two equal sides separately.
				// The value of |c^2 - 2 * b^2| + |c^2 - 2 * a^2| increases as dissimilarity
				// from isosceles right triangle.
				double d = std::abs(distAC - 2 * distAB) + std::abs(distAC - 2 * distBC);

				// Use cross product to figure out whether A and C are correct or flipped.
				// This asks whether BC x BA has a positive z component, which is the arrangement
				// we want for A, B, C. If it's negative then swap A and C.
				if (cross(*c - *b, *a - *b) < 0)
					std::swap(a, c);

				sets.emplace(d, FinderPatternSet{*a, *b, *c});

				// arbitrarily limit the number of potential sets
				if (sets.size() > 16)
					sets.erase(std::prev(sets.end()));
			}
		}
	}

	// convert from multimap to vector
	FinderPatternSets res;
	res.reserve(sets.size());
	for (auto& [d, s] : sets)
		res.push_back(s);
	return res;
}

static double EstimateModuleSize(const BitMatrix& image, PointF a, PointF b)
{
	BitMatrixCursorF cur(image, a, b - a);

	cur.stepToEdge(3);

	cur.turnBack();
	cur.step();
	assert(cur.isBlack());

	auto pattern = cur.readPattern<std::array<int, 4>>();

	return Reduce(pattern) / 6.0 * length(cur.d);
}

struct DimensionEstimate
{
	int dim = 0;
	double ms = 0;
	int err = 0;
};

static DimensionEstimate EstimateDimension(const BitMatrix& image, PointF a, PointF b)
{
	auto ms_a = EstimateModuleSize(image, a, b);
	auto ms_b = EstimateModuleSize(image, b, a);
	auto moduleSize = (ms_a + ms_b) / 2;

	int dimension = std::lround(distance(a, b) / moduleSize) + 7;
	int error     = 1 - (dimension % 4);

	return {dimension + error, moduleSize, std::abs(error)};
}

static RegressionLine TraceLine(const BitMatrix& image, PointF p, PointF d, int edge)
{
	BitMatrixCursorF cur(image, p, d - p);
	RegressionLine line;
	line.setDirectionInward(cur.back());

	cur.stepToEdge(edge);
	if (edge == 3) {
		// collect points inside the black line -> go one step back
		cur.turnBack();
		cur.step();
	}

	for (auto dir : {Direction::LEFT, Direction::RIGHT}) {
		auto c = BitMatrixCursorI(image, PointI(cur.p), PointI(mainDirection(cur.direction(dir))));
		// if cur.d is near diagonal, it could be c.p is at a corner, i.e. c is not currently at an edge and hence,
		// stepAlongEdge() would fail. Going either a step forward or backward should do the trick.
		if (!c.edgeAt(dir)) {
			c.step();
			if (!c.edgeAt(dir)) {
				c.step(-2);
				if (!c.edgeAt(dir))
					return {};
			}
		}

		auto stepCount = static_cast<int>(maxAbsComponent(cur.p - p));
		do {
			line.add(centered(c.p));
		} while (--stepCount > 0 && c.stepAlongEdge(dir, true));
	}

	line.evaluate(1.0);

	for (auto p : line.points())
		log(p, 2);

	return line;
}

static DetectorResult SampleAtFinderPatternSet(const BitMatrix& image, const FinderPatternSet& fp)
{
	auto top  = EstimateDimension(image, fp.tl, fp.tr);
	auto left = EstimateDimension(image, fp.tl, fp.bl);
	auto best = top.err < left.err ? top : left;
	int dimension = best.dim;
	int moduleSize = static_cast<int>(best.ms + 1);

	// generate 4 lines: outer and inner edge of the 1 module wide black line between the two outer and the inner
	// (tl) finder pattern
	auto bl2 = TraceLine(image, fp.bl, fp.tl, 2);
	auto bl3 = TraceLine(image, fp.bl, fp.tl, 3);
	auto tr2 = TraceLine(image, fp.tr, fp.tl, 2);
	auto tr3 = TraceLine(image, fp.tr, fp.tl, 3);

	auto quad = Rectangle(dimension, dimension, 3.5);
	PointF br = fp.tr - fp.tl + fp.bl;

	if (bl2.isValid() && tr2.isValid() && bl3.isValid() && tr3.isValid()) {
		// intersect both outer and inner line pairs and take the center point between the two intersection points
		br = (intersect(bl2, tr2) + intersect(bl3, tr3)) / 2;

		// if the estimated alignment pattern position is outside of the image, stop here
		if (!image.isIn(PointI(br), 3 * moduleSize))
			return {};

		log(br, 3);
		quad[2] = quad[2] - PointF(3, 3);

		// Everything except version 1 (21 modules) has an alignment pattern
		if (dimension > 21) {
			// in case we landed outside of the central black module of the alignment pattern, use the center
			// of the next best circle (either outer or inner edge of the white part of the alignment pattern)
			auto br2 = CenterOfRing(image, PointI(br), moduleSize * 4, 1, false).value_or(br);
			// if we did not land on a black pixel or the concentric pattern finder fails,
			// leave the intersection of the lines as the best guess
			if (image.get(br2))
				br = LocateConcentricPattern<true>(image, FixedPattern<3, 3>{1, 1, 1}, br2, moduleSize * 3)
						 .value_or(ConcentricPattern{br});
		}
	}

	return SampleGrid(image, dimension, dimension, {quad, {fp.tl, fp.tr, br, fp.bl}});
}

/**
* This method detects a code in a "pure" image -- that is, pure monochrome image
* which contains only an unrotated, unskewed, image of a code, with some white border
* around it. This is a specialized method that works exceptionally fast in this special
* case.
*/
static DetectorResult DetectPure(const BitMatrix& image)
{
	const int minSize = 21; // Number of modules in the smallest QRCode (Version 1)
	int left, top, width, height;
	if (!image.findBoundingBox(left, top, width, height, minSize) || width != height) {
		return {};
	}

	// find the first white pixel on the diagonal
	int moduleSize = 1;
	while (moduleSize < width / minSize && image.get(left + moduleSize, top + moduleSize))
		++moduleSize;

	int matrixWidth = width / moduleSize;
	int matrixHeight = height / moduleSize;
	if (matrixWidth < minSize || matrixHeight < minSize) {
		return {};
	}

	// Push in the "border" by half the module width so that we start
	// sampling in the middle of the module. Just in case the image is a
	// little off, this will help recover.
	int msh    = moduleSize / 2;
	int right  = left + width - 1;
	int bottom = top + height - 1;

	// Now just read off the bits (this is a crop + subsample)
	return {Deflate(image, matrixWidth, matrixHeight, top + msh, left + msh, moduleSize),
			{{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
}

DetectorResult Detector::Detect(const BitMatrix& image, bool tryHarder, bool isPure)
{
#ifdef PRINT_DEBUG
	LogMatrixWriter lmw(log, image, 5, "qr-log.pnm");
#endif

	if (isPure)
		return DetectPure(image);

	auto sets = GenerateFinderPatternSets(FindFinderPatterns(image, tryHarder));

	if (sets.empty())
		return {};

#ifdef PRINT_DEBUG
	printf("size of sets: %d\n", Size(sets));
#endif

	return SampleAtFinderPatternSet(image, sets[0]);
}

} // QRCode
} // ZXing
