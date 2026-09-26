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
    std::fprintf(f, "# unknown records and quad keys are ignored by the parser and written back as-is.\n");
    if (m.cube && m.quads.empty()) std::fprintf(f, "cube\n");
    for (const std::string& s : m.extraLines) std::fprintf(f, "%s\n", s.c_str());
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
