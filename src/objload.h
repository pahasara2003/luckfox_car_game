// Wavefront OBJ loader for engine3d: triangulates polygons, reads per-face colour (Kd) and
// texture (map_Kd, binary PPM with power-of-two sides) from the .mtl file.
// Convert other formats first:  assimp export model.glb model.obj
// and textures:                 magick tex.png -resize 128x128! tex.ppm   (then point map_Kd at tex.ppm)
#pragma once
#include "engine3d.h"
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

// Binary PPM (P6, maxval 255) to an RGB565 texture.
inline std::shared_ptr<Texture> loadPpm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open texture " + path);
    std::string magic; int w = 0, h = 0, maxv = 0;
    auto skip = [&] { in >> std::ws; while (in.peek() == '#') { std::string c; std::getline(in, c); in >> std::ws; } };
    in >> magic; skip(); in >> w; skip(); in >> h; skip(); in >> maxv;
    in.get();
    if (magic != "P6" || maxv != 255) throw std::runtime_error(path + ": need a binary 8-bit PPM (magick in.png out.ppm)");
    auto t = std::make_shared<Texture>(w, h);   // throws unless power-of-two
    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    if (!in.read(reinterpret_cast<char*>(rgb.data()), rgb.size())) throw std::runtime_error(path + ": truncated");
    for (size_t i = 0; i < t->px.size(); ++i) t->px[i] = rgb565(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]);
    return t;
}

struct Material { uint16_t color; std::shared_ptr<Texture> tex; };

inline std::map<std::string, Material> loadMtl(const std::string& path) {
    std::map<std::string, Material> out;
    std::ifstream in(path);
    std::string dir = path.substr(0, path.find_last_of('/') + 1);
    std::string line, name;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key; ls >> key;
        if (key == "newmtl") { ls >> name; out[name] = {0xFFFF, nullptr}; }
        else if (key == "Kd") {
            float r = 1, g = 1, b = 1; ls >> r >> g >> b;
            auto q = [](float v, int m) { return std::max(0, std::min(m, int(v * m + 0.5f))); };
            out[name].color = uint16_t(q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31));
        } else if (key == "map_Kd") {
            std::string f; std::getline(ls >> std::ws, f);
            if (!f.empty() && f.back() == '\r') f.pop_back();
            out[name].tex = loadPpm(f[0] == '/' ? f : dir + f);
        }
    }
    return out;
}

// Loads an OBJ, centres it and scales its largest side to `size`. Faces without a material get `color`.
inline Mesh loadObj(const std::string& path, float size = 2, uint16_t color = 0xFFFF) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string dir = path.substr(0, path.find_last_of('/') + 1);
    std::map<std::string, Material> mtl;
    Material cur{color, nullptr};
    Mesh m;
    std::vector<Vec2> uvs;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key; ls >> key;
        if (key == "v") {
            Vec3 p; ls >> p.x >> p.y >> p.z;
            p.z = -p.z;                                   // OBJ is right-handed, engine looks down +z
            m.v.push_back(p);
        } else if (key == "vt") {
            Vec2 t{0, 0}; ls >> t.u >> t.v;
            uvs.push_back({t.u, 1 - t.v});                // OBJ v=0 is the bottom row of the image
        } else if (key == "f") {
            std::vector<int> idx, tix; std::string tok;
            while (ls >> tok) {                           // "v", "v/vt", "v//vn", "v/vt/vn"
                int i = std::stoi(tok);
                idx.push_back(i < 0 ? int(m.v.size()) + i : i - 1);
                size_t s = tok.find('/');
                int ti = s != std::string::npos && s + 1 < tok.size() && tok[s + 1] != '/' ? std::stoi(tok.substr(s + 1)) : 0;
                tix.push_back(ti < 0 ? int(uvs.size()) + ti : ti - 1);
            }
            for (size_t k = 1; k + 1 < idx.size(); ++k) {
                size_t order[3] = {0, k + 1, k};          // reversed: z flip mirrors the winding
                Tri t{idx[0], idx[k + 1], idx[k], cur.color, nullptr, {}};
                for (int j = 0; j < 3; ++j) {
                    int vi = idx[order[j]];
                    if (vi < 0 || vi >= int(m.v.size())) throw std::runtime_error("bad face index in " + path);
                    int ti = tix[order[j]];
                    if (cur.tex && ti >= 0 && ti < int(uvs.size())) { t.tex = cur.tex.get(); t.uv[j] = uvs[ti]; }
                }
                if (t.tex) t.color = 0xFFFF;
                m.t.push_back(t);
            }
        } else if (key == "mtllib") {
            std::string f; std::getline(ls >> std::ws, f);
            if (!f.empty() && f.back() == '\r') f.pop_back();
            mtl = loadMtl(dir + f);
            for (auto& kv : mtl) if (kv.second.tex) m.textures.push_back(kv.second.tex);
        } else if (key == "usemtl") {
            std::string n; ls >> n;
            cur = mtl.count(n) ? mtl[n] : Material{color, nullptr};
        }
    }
    if (m.v.empty()) throw std::runtime_error("no vertices in " + path);
    Vec3 lo = m.v[0], hi = m.v[0];
    for (auto& p : m.v) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    float ext = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    float s = ext > 0 ? size / ext : 1;
    Vec3 c{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
    for (auto& p : m.v) p = {(p.x - c.x) * s, (p.y - c.y) * s, (p.z - c.z) * s};
    return m;
}
