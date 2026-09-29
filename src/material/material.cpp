#include "material.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

namespace mat {

namespace {
// Trim leading/trailing whitespace and strip a trailing '\r'.
std::string trim(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    const char* e = s + std::strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    return std::string(s, (size_t)(e - s));
}
} // namespace

Model loadModel(const char* path) {
    Model m;
    FILE* f = std::fopen(path, "rb");
    if (!f) return m;

    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;

        std::vector<std::string> toks;
        {
            char buf[2048];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            char* tok = std::strtok(buf, " \t");
            while (tok) {
                toks.emplace_back(tok);
                tok = std::strtok(nullptr, " \t");
            }
        }
        if (toks.empty()) continue;
        if (toks[0] == "cube") {
            m.cube = true;
            continue;
        }
        if (toks[0] == "part") {
            if (toks.size() >= 10) {
                Solid p;
                for (int k = 0; k < 3; k++) {
                    p.c[k] = (float)std::atof(toks[1 + k].c_str());
                    p.h[k] = (float)std::atof(toks[4 + k].c_str());
                    p.rgb[k] = (float)std::atof(toks[7 + k].c_str());
                }
                for (int k = 0; k < 3; k++) if (p.h[k] < 1e-4f) p.h[k] = 1e-4f;
                for (size_t i = 10; i < toks.size(); i++) {
                    const std::string& e = toks[i];
                    if (e == "tex" && i + 1 < toks.size()) p.tex = toks[++i];
                    else if (e == "bind" && i + 1 < toks.size()) p.bind = std::atoi(toks[++i].c_str());
                    else if (e == "card") p.kind = 1;
                    else if (e == "crop") p.crop = true;
                    else if (e == "texs" && i + 1 < toks.size()) p.texScale = (float)std::atof(toks[++i].c_str());
                    else if (e == "face" && i + 1 < toks.size()) p.face = std::atoi(toks[++i].c_str());
                    else if (e == "uvf" && i + 1 < toks.size()) p.uvFlip = std::atoi(toks[++i].c_str());
                    else if (e == "rot" && i + 3 < toks.size()) {
                        p.rot[0] = (float)std::atof(toks[++i].c_str());
                        p.rot[1] = (float)std::atof(toks[++i].c_str());
                        p.rot[2] = (float)std::atof(toks[++i].c_str());
                    } else if (e == "box" && i + 5 < toks.size()) {
                        p.boxX = std::atoi(toks[++i].c_str());
                        p.boxY = std::atoi(toks[++i].c_str());
                        p.boxW = std::atoi(toks[++i].c_str());
                        p.boxH = std::atoi(toks[++i].c_str());
                        p.boxD = std::atoi(toks[++i].c_str());
                    }
                }
                m.solids.push_back(p);
            } else {
                m.extraLines.push_back(s);
            }
            continue;
        }
        if (toks[0] != "quad") {
            m.extraLines.push_back(s);
            continue;
        }

        Quad q;
        int n = 0;
        std::vector<std::string> extra;
        for (size_t i = 1; i < toks.size(); i++) {
            if (n < 20) {
                float v = (float)std::atof(toks[i].c_str());
                int corner = n / 5;
                int field = n % 5;
                if (field < 3) q.p[corner][field] = v;
                else q.uv[corner][field - 3] = v;
            } else {
                extra.push_back(toks[i]);
            }
            n++;
        }
        if (n < 20) {
            m.extraLines.push_back(s);
            continue;
        }
        for (size_t i = 0; i < extra.size(); i++) {
            const std::string& e = extra[i];
            if (e == "double") q.doubleSided = true;
            else if (e == "crop") { q.crop = true; q.solid = true; }
            else if (e == "solid") q.solid = true;
            else if (e == "uvlock") q.uvLock = true;
            else if (e == "fill") q.uvMode = 1;
            else if (e == "cropuv") q.uvMode = 2;
            else if (e == "tex" && i + 1 < extra.size()) q.tex = extra[++i];
            else if (e == "group" && i + 1 < extra.size()) q.group = std::atoi(extra[++i].c_str());
            else if (e == "bind" && i + 1 < extra.size()) q.bind = std::atoi(extra[++i].c_str());
            else if (e == "face" && i + 1 < extra.size()) q.face = std::atoi(extra[++i].c_str());
            else if (e == "uvmode" && i + 1 < extra.size()) q.uvMode = std::atoi(extra[++i].c_str());
            else if (e == "texs" && i + 1 < extra.size()) q.texScale = (float)std::atof(extra[++i].c_str());
            else {
                std::string v;
                if (i + 1 < extra.size() && extra[i + 1] != "double" && extra[i + 1] != "crop"
                    && extra[i + 1] != "solid" && extra[i + 1] != "uvlock" && extra[i + 1] != "fill"
                    && extra[i + 1] != "cropuv" && extra[i + 1] != "tex" && extra[i + 1] != "group"
                    && extra[i + 1] != "bind" && extra[i + 1] != "face" && extra[i + 1] != "uvmode"
                    && extra[i + 1] != "texs") {
                    v = extra[++i];
                }
                q.extra.push_back({ e, v });
            }
        }
        m.quads.push_back(q);
    }
    std::fclose(f);
    return m;
}

bool saveModel(const char* path, const Model& m) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "# Material model (quad x y z u v per corner; optional flags / key value).\n");
    std::fprintf(f, "# part cx cy cz hx hy hz r g b [tex name] [bind n] [card] [crop] [texs s] [face n] [uvf n] [rot x y z] [box x y w h d]\n");
    std::fprintf(f, "# unknown records and quad keys are ignored by the parser and written back as-is.\n");
    if (m.cube && m.quads.empty()) std::fprintf(f, "cube\n");
    for (const std::string& s : m.extraLines) std::fprintf(f, "%s\n", s.c_str());
    for (const Solid& p : m.solids) {
        std::fprintf(f, "part %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                     p.c[0], p.c[1], p.c[2], p.h[0], p.h[1], p.h[2],
                     p.rgb[0], p.rgb[1], p.rgb[2]);
        if (!p.tex.empty()) std::fprintf(f, " tex %s", p.tex.c_str());
        if (p.bind >= 0) std::fprintf(f, " bind %d", p.bind);
        if (p.kind == 1) std::fprintf(f, " card");
        if (p.crop) std::fprintf(f, " crop");
        if (std::fabs(p.texScale - 1.0f) > 1e-4f) std::fprintf(f, " texs %.4f", p.texScale);
        if (p.kind == 1 && p.face != 5) std::fprintf(f, " face %d", p.face);
        if (p.uvFlip) std::fprintf(f, " uvf %d", p.uvFlip);
        if (solidRotated(p)) std::fprintf(f, " rot %.4f %.4f %.4f", p.rot[0], p.rot[1], p.rot[2]);
        if (solidHasBox(p))
            std::fprintf(f, " box %d %d %d %d %d", p.boxX, p.boxY, p.boxW, p.boxH, p.boxD);
        std::fprintf(f, "\n");
    }
    for (const Quad& q : m.quads) {
        std::fprintf(f, "quad");
        for (int c = 0; c < 4; c++) {
            std::fprintf(f, " %.4f %.4f %.4f %.4f %.4f",
                         q.p[c][0], q.p[c][1], q.p[c][2], q.uv[c][0], q.uv[c][1]);
        }
        if (q.doubleSided) std::fprintf(f, " double");
        if (q.crop) std::fprintf(f, " crop");
        if (q.solid && !q.crop) std::fprintf(f, " solid");
        if (q.uvMode == 1) std::fprintf(f, " fill");
        else if (q.uvMode == 2) std::fprintf(f, " cropuv");
        else if (q.uvMode != 0) std::fprintf(f, " uvmode %d", q.uvMode);
        if (q.uvLock) std::fprintf(f, " uvlock");
        if (q.texScale > 1e-4f && std::fabs(q.texScale - 1.0f) > 1e-4f)
            std::fprintf(f, " texs %.4f", q.texScale);
        if (!q.tex.empty()) std::fprintf(f, " tex %s", q.tex.c_str());
        if (q.group >= 0) std::fprintf(f, " group %d", q.group);
        if (q.bind >= 0) std::fprintf(f, " bind %d", q.bind);
        if (q.face >= 0) std::fprintf(f, " face %d", q.face);
        for (const auto& kv : q.extra) {
            if (kv.second.empty()) std::fprintf(f, " %s", kv.first.c_str());
            else std::fprintf(f, " %s %s", kv.first.c_str(), kv.second.c_str());
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

bool subtractSolid(std::vector<Solid>& parts, int index, const float cmn[3], const float cmx[3]) {
    if (index < 0 || index >= (int)parts.size()) return false;
    Solid src = parts[index];
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) {
        mn[k] = src.c[k] - src.h[k];
        mx[k] = src.c[k] + src.h[k];
    }
    float imn[3], imx[3];
    const float eps = 1e-4f;
    for (int k = 0; k < 3; k++) {
        imn[k] = std::max(mn[k], cmn[k]);
        imx[k] = std::min(mx[k], cmx[k]);
        if (imx[k] - imn[k] <= eps) return false;
    }
    parts.erase(parts.begin() + index);
    auto emit = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        if (x1 - x0 <= eps || y1 - y0 <= eps || z1 - z0 <= eps) return;
        Solid p = src;
        p.c[0] = (x0 + x1) * 0.5f; p.c[1] = (y0 + y1) * 0.5f; p.c[2] = (z0 + z1) * 0.5f;
        p.h[0] = (x1 - x0) * 0.5f; p.h[1] = (y1 - y0) * 0.5f; p.h[2] = (z1 - z0) * 0.5f;
        parts.push_back(p);
    };
    emit(mn[0], mn[1], mn[2], imn[0], mx[1], mx[2]);
    emit(imx[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    emit(imn[0], mn[1], mn[2], imx[0], imn[1], mx[2]);
    emit(imn[0], imx[1], mn[2], imx[0], mx[1], mx[2]);
    emit(imn[0], imn[1], mn[2], imx[0], imx[1], imn[2]);
    emit(imn[0], imn[1], imx[2], imx[0], imx[1], mx[2]);
    return true;
}

RandParams loadRand(const char* path) {
    RandParams r;
    FILE* f = std::fopen(path, "rb");
    if (!f) return r;

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        char key[64];
        float val = 0.0f;
        if (std::sscanf(s.c_str(), "%63s %f", key, &val) == 2) {
            r.f[std::string(key)] = val;
        } else {
            r.extraLines.push_back(s);
        }
    }
    std::fclose(f);
    return r;
}

bool saveRand(const char* path, const RandParams& r) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "# Material randomization parameters.\n");
    std::fprintf(f, "# unknown lines are ignored by the parser and written back as-is.\n");
    std::vector<std::string> keys;
    keys.reserve(r.f.size());
    for (const auto& kv : r.f) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (const std::string& k : keys) {
        std::fprintf(f, "%s %g\n", k.c_str(), r.f.at(k));
    }
    for (const std::string& s : r.extraLines) std::fprintf(f, "%s\n", s.c_str());
    std::fclose(f);
    return true;
}

} // namespace mat
