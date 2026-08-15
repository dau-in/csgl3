#include "stdafx.h"
#include "lightgamma.h"

namespace Render
{

struct CurveFit
{
    float wh; // hinge position in sqrt space, = sqrt(x) where B*x^L == G3
    float wsat; // saturation point in sqrt space, = sqrt(x) where B*x^L == 1
    float p[3]; // w <= wh, y = p[0]*w + p[1]*w^2 + p[2]*w^3
    float q[3]; // w > wh, y = p(w) + q[0]*t + q[1]*t^2 + q[2]*t^3, t = w - wh
};

static double Det3(const double m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
        - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Cramer's rule is fine, no need to worry about singularities
static void Solve3x3(const double a[3][3], const double b[3], double x[3])
{
    double invDet = 1.0 / Det3(a);

    for (int i = 0; i < 3; i++)
    {
        double m[3][3];
        memcpy(m, a, sizeof(m));

        for (int j = 0; j < 3; j++)
        {
            m[j][i] = b[j];
        }

        x[i] = Det3(m) * invDet;
    }
}

// zero out insignificant terms so we don't emit the full horner form or hinge crap when it's not needed
static void SnapNoise(double c[3], double span)
{
    // FIXME: bullshit
    constexpr double Epsilon = 1e-7;

    double s = span;

    for (int i = 0; i < 3; ++i)
    {
        if (fabs(c[i]) * s < Epsilon)
        {
            c[i] = 0.0;
        }

        s *= span;
    }
}

static CurveFit FitCurve(float gamma, float lightgamma, float brightness)
{
    double L = (double)lightgamma;
    double G = 1.0 / gamma;

    // from BuildGammaTable
    double G3, B;
    if (brightness <= 0.0)
    {
        G3 = 0.125;
        B = 1.0;
    }
    else if (brightness > 1.0)
    {
        G3 = 0.05;
        B = brightness;
    }
    else
    {
        G3 = 0.125 - (brightness * brightness) * 0.075;
        B = 1.0;
    }

    // identity when brightness <= 0 and gamma == lightgamma, but
    // who the fuck plays with brightness that low... don't handle it any different

    // hinge and saturation point in w = sqrt(x)
    double wh = pow(G3 / B, 0.5 / L);
    double wsat = pow(B, -0.5 / L);

    double T = wsat - wh;

    // the two branches expressed in w
    double loC = pow(0.125 * B / G3, G);
    double loE = 2.0 * L * G;

    // Chebyshev-Gauss-Lobatto points, cos(pi*k/3) for k=0..3, mapped to [0,1] via (1-cos)/2
    // we can leave out k=0 node because neither basis has a constant term
    double nodes[3] = { 0.25, 0.75, 1.0 };

    double a[3][3], b[3], p[3], q[3];

    // interpolate lower branch on [0, wh]
    for (int i = 0; i < 3; ++i)
    {
        double n = wh * nodes[i];

        a[i][0] = n;
        a[i][1] = n * n;
        a[i][2] = n * n * n;
        b[i] = loC * pow(n, loE);
    }

    Solve3x3(a, b, p);
    SnapNoise(p, wh);

    // interpolate upper branch on [0, T] as a correction on top of p
    for (int i = 0; i < 3; ++i)
    {
        double t = T * nodes[i];
        double w = (i == 2) ? wsat : wh + t;
        double hi = (i == 2) ? 1.0 : pow(0.125 + ((B * pow(w, 2.0 * L) - G3) / (1.0 - G3)) * 0.875, G);

        a[i][0] = t;
        a[i][1] = t * t;
        a[i][2] = t * t * t;
        b[i] = hi - w * (p[0] + w * (p[1] + w * p[2]));
    }

    Solve3x3(a, b, q);
    SnapNoise(q, T);

    CurveFit result{};

    result.wh = (float)wh;
    result.wsat = (float)wsat;

    for (int i = 0; i < 3; ++i)
    {
        result.p[i] = (float)p[i];
        result.q[i] = (float)q[i];
    }

    return result;
}

static void Appendf(std::string &dest, const char *fmt, ...)
{
    va_list ap;
    char temp[1024];

    va_start(ap, fmt);
    int result = vsnprintf(temp, sizeof(temp), fmt, ap);
    va_end(ap);

    GL3_ASSERT(result > 0 && result < (int)sizeof(temp));
    dest.append(temp);
}

static bool HasTerms(const float c[3])
{
    return c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f;
}

static void AppendHorner(std::string &dest, const float c[3], const char *v)
{
    int idx[3], n = 0;

    // only emit nonzero terms
    for (int i = 0; i < 3; ++i)
    {
        if (c[i] != 0.0f)
        {
            idx[n++] = i;
        }
    }

    if (n == 0)
    {
        return;
    }

    int prev = -1;

    for (int k = 0; k < n; ++k)
    {
        for (int p = 0; p < idx[k] - prev; ++p)
        {
            Appendf(dest, "%s * ", v);
        }

        if (k + 1 < n)
        {
            dest.append("(");
        }

        Appendf(dest, "%.9g", c[idx[k]]);

        if (k + 1 < n)
        {
            dest.append(" + ");
        }

        prev = idx[k];
    }

    dest.append(n - 1, ')');
}

std::string LightGammaGLSL(float gamma, float lightgamma, float brightness)
{
    // can be over 1024 currently
    constexpr size_t MaxSize = 2048;

    std::string result;
    result.reserve(MaxSize);

    CurveFit f = FitCurve(gamma, lightgamma, brightness);

    bool hasP = HasTerms(f.p);
    bool hasQ = HasTerms(f.q);

    for (const char *type : { "float", "vec3" })
    {
        Appendf(result, "%s ApplyBrightness(%s x)\n{\n", type, type);

        if (hasQ)
        {
            Appendf(result, "const float wh = %.9g;\n", f.wh);
        }

        Appendf(result, "const float wsat = %.9g;\n", f.wsat);

        Appendf(result, "%s w = min(sqrt(x), wsat);\n", type);

        if (hasQ)
        {
            Appendf(result, "%s t = max(w - wh, 0.0);\n", type);
        }

        Appendf(result, "return clamp(");

        if (hasP)
        {
            AppendHorner(result, f.p, "w");
        }

        if (hasP && hasQ)
        {
            Appendf(result, " + ");
        }

        if (hasQ)
        {
            AppendHorner(result, f.q, "t");
        }

        if (!hasP && !hasQ)
        {
            Appendf(result, "0.0");
        }

        Appendf(result, ", 0.0, 1.0);\n}\n");
    }

    GL3_ASSERT(result.size() < MaxSize);
    return result;
}

}
