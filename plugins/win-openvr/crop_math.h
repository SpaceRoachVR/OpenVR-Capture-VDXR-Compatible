#pragma once

// Crop rectangle for the capture source: pure math, no OBS dependency, so it
// can be unit-tested (tests/crop_math_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vrcapture {

struct CropRect {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

// Where the eye's view direction lands in its image, as fractions of the
// image size (0.5, 0.5 = image center). Headset eye images are not centered
// on the view direction: the lens FOV is asymmetric (wider toward the outside
// of the face), so this is computed from the eye's FOV tangents.
struct OpticalCenter {
	double x = 0.5;
	double y = 0.5;
};

// From FOV tangents in OpenXR convention: tanLeft < 0 < tanRight,
// tanDown < 0 < tanUp. Image y grows downward.
inline OpticalCenter OpticalCenterFromTangents(double tanLeft, double tanRight, double tanUp, double tanDown)
{
	OpticalCenter c;
	if (tanRight - tanLeft > 1e-6) {
		c.x = std::clamp(-tanLeft / (tanRight - tanLeft), 0.0, 1.0);
	}
	if (tanUp - tanDown > 1e-6) {
		c.y = std::clamp(tanUp / (tanUp - tanDown), 0.0, 1.0);
	}
	return c;
}

// deviceW/H:  eye image size.
// zoom:       >= 1; the crop spans 1/zoom of the image in each direction
//             before the aspect ratio is applied.
// aspect:     target width/height, or <= 0 for the image's native aspect.
//             The rectangle is the largest one of that aspect that fits in
//             the zoomed area, i.e. it keeps as much of the FOV as possible.
// center:     where the crop is centered before offsets (see OpticalCenter).
// offsetX/Y:  pixel offsets applied after centering; the result is clamped
//             to stay inside the image.
inline CropRect ComputeCrop(uint32_t deviceW, uint32_t deviceH, double zoom, double aspect, OpticalCenter center,
			    int64_t offsetX, int64_t offsetY)
{
	CropRect r;
	if (deviceW == 0 || deviceH == 0) {
		return r;
	}

	zoom = (std::max)(1.0, std::isfinite(zoom) ? zoom : 1.0);
	double w = static_cast<double>(deviceW) / zoom;
	double h = static_cast<double>(deviceH) / zoom;

	if (aspect > 0.0 && std::isfinite(aspect)) {
		if (w / h > aspect) {
			w = h * aspect; // area is too wide: trim the sides
		} else {
			h = w / aspect; // area is too tall: trim top and bottom
		}
	}

	const int64_t cw = std::clamp<int64_t>(std::llround(w), 1, deviceW);
	const int64_t ch = std::clamp<int64_t>(std::llround(h), 1, deviceH);

	int64_t x = std::llround(center.x * deviceW - cw / 2.0) + offsetX;
	int64_t y = std::llround(center.y * deviceH - ch / 2.0) + offsetY;
	x = std::clamp<int64_t>(x, 0, static_cast<int64_t>(deviceW) - cw);
	y = std::clamp<int64_t>(y, 0, static_cast<int64_t>(deviceH) - ch);

	r.x = static_cast<uint32_t>(x);
	r.y = static_cast<uint32_t>(y);
	r.width = static_cast<uint32_t>(cw);
	r.height = static_cast<uint32_t>(ch);
	return r;
}

} // namespace vrcapture
