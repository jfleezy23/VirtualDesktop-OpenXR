// Independent experiment using Bjorn Ottosson's published Oklab matrices.
// https://bottosson.github.io/posts/oklab/
float3 LinearToPerceptual(float3 rgb) {
    float3 cone = float3(dot(rgb, float3(0.4122214708, 0.5363325363, 0.0514459929)),
                         dot(rgb, float3(0.2119034982, 0.6806995451, 0.1073969566)),
                         dot(rgb, float3(0.0883024619, 0.2817188376, 0.6299787005)));
    cone = pow(max(cone, 0.0), 1.0 / 3.0);
    return float3(dot(cone, float3(0.2104542553, 0.7936177850, -0.0040720468)),
                  dot(cone, float3(1.9779984951, -2.4285922050, 0.4505937099)),
                  dot(cone, float3(0.0259040371, 0.7827717662, -0.8086757660)));
}

float3 PerceptualToLinear(float3 lab) {
    float3 cone = float3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                         lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                         lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    cone = cone * cone * cone;
    return float3(dot(cone, float3(4.0767416621, -3.3077115913, 0.2309699292)),
                  dot(cone, float3(-1.2684380046, 2.6097574011, -0.3413193965)),
                  dot(cone, float3(-0.0041960863, -0.7034186147, 1.7076147010)));
}

float3 TransferPerceptualEdit(float3 nativeLinear, float3 nativeLab, float3 guideLinear, float3 answerLinear) {
    float3 guideLab = LinearToPerceptual(guideLinear);
    float3 answerLab = LinearToPerceptual(answerLinear);
    float denominator = max(nativeLab.x, guideLab.x) + 0.0001;
    float ratio = 1.0 + (answerLab.x - guideLab.x) / denominator;
    float chromaScale = min(nativeLab.x, guideLab.x) / denominator;
    float3 transferred = float3(nativeLab.x * ratio, nativeLab.yz + (answerLab.yz - guideLab.yz) * chromaScale);
    // Keep signed out-of-gamut edits until existing protection and final storage.
    return PerceptualToLinear(transferred) - nativeLinear;
}
