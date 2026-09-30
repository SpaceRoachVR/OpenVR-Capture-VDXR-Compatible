// Unit tests for the capture crop rectangle (plugins/win-openvr/crop_math.h).

#include "crop_math.h"
#include "test_util.h"

using namespace vrcapture;
using namespace vrcapture_test;

static bool Is(const CropRect &r, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (r.x == x && r.y == y && r.width == w && r.height == h) return true;
	std::printf("        got x=%u y=%u w=%u h=%u, want x=%u y=%u w=%u h=%u\n", r.x, r.y, r.width, r.height, x, y, w,
		    h);
	return false;
}

int main()
{
	const OpticalCenter mid;
	const double k16x9 = 16.0 / 9.0;

	std::printf("1. native keeps the whole image\n");
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, -1.0, mid, 0, 0), 0, 0, 2112, 2304));

	std::printf("2. 16:9 on a tall eye image keeps full width (largest FOV), centered vertically\n");
	// 2112 / (16/9) = 1188; centered: (2304 - 1188) / 2 = 558
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, k16x9, mid, 0, 0), 0, 558, 2112, 1188));

	std::printf("3. 16:9 on a wide image keeps full height\n");
	// 1000 * 16/9 = 1778; centered: (4000 - 1778) / 2 = 1111
	CHECK(Is(ComputeCrop(4000, 1000, 1.0, k16x9, mid, 0, 0), 1111, 0, 1778, 1000));

	std::printf("4. crop follows the eye's optical center, clamped to the image\n");
	const OpticalCenter low{0.5, 0.6};
	// center y = 0.6 * 2304 = 1382.4 -> top = 1382.4 - 594 = 788
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, k16x9, low, 0, 0), 0, 788, 2112, 1188));
	const OpticalCenter edge{0.5, 0.95};
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, k16x9, edge, 0, 0), 0, 2304 - 1188, 2112, 1188));

	std::printf("5. offsets move the crop, clamped\n");
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, k16x9, mid, 0, -100), 0, 458, 2112, 1188));
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, k16x9, mid, 0, -10000), 0, 0, 2112, 1188));
	CHECK(Is(ComputeCrop(2112, 2304, 1.0, k16x9, mid, 500, 0), 0, 558, 2112, 1188)); // no room horizontally

	std::printf("6. zoom shrinks the area around the optical center\n");
	const OpticalCenter off{0.4, 0.5};
	// zoom 2: 1056x1152 -> 16:9 -> 1056x594; center x = 844.8 -> left = 316.8 -> 317
	CHECK(Is(ComputeCrop(2112, 2304, 2.0, k16x9, off, 0, 0), 317, 855, 1056, 594));

	std::printf("7. optical center from FOV tangents\n");
	// Symmetric FOV -> image center.
	OpticalCenter c = OpticalCenterFromTangents(-1.0, 1.0, 1.0, -1.0);
	CHECK(std::fabs(c.x - 0.5) < 1e-9 && std::fabs(c.y - 0.5) < 1e-9);
	// Left eye, wider toward the outside (left): center shifts right.
	c = OpticalCenterFromTangents(-1.4, 0.8, 1.0, -1.2);
	CHECK(std::fabs(c.x - 1.4 / 2.2) < 1e-9); // 0.636
	CHECK(std::fabs(c.y - 1.0 / 2.2) < 1e-9); // 0.455: more FOV below than above
	// Degenerate input falls back to the image center.
	c = OpticalCenterFromTangents(0, 0, 0, 0);
	CHECK(c.x == 0.5 && c.y == 0.5);

	std::printf("8. degenerate inputs\n");
	CHECK(Is(ComputeCrop(0, 0, 1.0, k16x9, mid, 0, 0), 0, 0, 0, 0));
	CHECK(Is(ComputeCrop(100, 100, 0.2, -1.0, mid, 0, 0), 0, 0, 100, 100)); // zoom < 1 treated as 1

	return Finish();
}
