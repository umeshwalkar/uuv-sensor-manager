#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <nlohmann/json.hpp>
#include "Logger.hpp"

/**
 * Valeport SVP device data — three output formats supported:
 *
 *   "nmea_psgds"  — "$PSGDS,ADSVP,P,SV,T,D*HH"
 *
 */
struct SensorData
{
    // Device-supplied timestamp
    std::string date; // yyyy-mm-dd
    std::string time; // hh:mm:ss.sss

    // Core SVP fields — populated for all formats
    double parameter_1_value = 0.0; // unit
    double parameter_2_value = 0.0; // unit
    double parameter_n_value = 0.0; // unit
};

class SensorParser
{
public:
    /**
     * Auto-detect format and parse packet into data.
     * Returns false if the line is unrecognised or malformed.
     */
    static bool parse(const std::string &packet, SensorData &data)
    {
        std::string s = packet;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
        if (s.empty())
            return false;

        data = SensorData{};
        // data.raw = s;

        if (s[0] == '$')
            return parseNmea(s, data);

        return parseproperitary(s, data);
    }

    /**
     * Serialise data to JSON, injecting the epoch receive timestamp (seconds
     * since Unix epoch) as "ts".  Pass ts_epoch = -1 to omit the field.
     */
    static std::string toJSON(const SensorData &data, double ts_epoch = -1.0)
    {
        nlohmann::json j;

        if (ts_epoch >= 0.0)
            j["ts"] = ts_epoch;

        j["param1"] = data.parameter_1_value;
        j["param2"] = data.parameter_2_value;
        j["paramn"] = data.parameter_n_value;

        return j.dump();
    }

private:
    // ── Format 1: $PSGDS,param1,param2,param3*HH ─────────────
    static bool parseNmea(const std::string &s, SensorData &data)
    {
        // Strip optional checksum
        std::string body = s;
        size_t star = body.find('*');
        if (star != std::string::npos)
            body = body.substr(0, star);

        auto f = split(body, ',');
        if (f.size() < 5)
        {
            LOG_WRN("SensorParser", "NMEA: need >=5 fields, got %zu in: %s",
                    f.size(), s.c_str());
            return false;
        }
        if (f.size() < 2 || f[1] != "ADSVP")
        {
            LOG_WRN("SensorParser", "NMEA: unexpected sentence type in: %s", s.c_str());
            return false;
        }

        try
        {
            data.parameter_1_value = parseDouble(f[2]);
            data.parameter_2_value = parseDouble(f[3]);
            data.parameter_n_value = parseDouble(f[4]);
          
            return true;
        }
        catch (const std::exception &e)
        {
            LOG_ERR("SensorParser", "NMEA parse error: %s  line: %s",
                    e.what(), s.c_str());
            return false;
        }
    }

    // ── Format 2: whitespace-delimited "param1 param2 param3" ─────────────────────────
    static bool parseproperitary(const std::string &s, SensorData &data)
    {
        std::istringstream ss(s);
        std::vector<double> v;
        std::string tok;
        while (ss >> tok)
        {
            try
            {
                v.push_back(std::stod(tok));
            }
            catch (...)
            { /* skip non-numeric tokens */
            }
        }
        if (v.size() < 3)
        {
            LOG_WRN("SensorParser", "Whitespace: need >=3 numeric fields, got %zu in: %s",
                    v.size(), s.c_str());
            return false;
        }

        data.parameter_1_value = v[0];
        data.parameter_2_value = v[1];
        data.parameter_n_value = v[2];

        return true;
    }

    static double parseDouble(const std::string &s) { return std::stod(s); }

    static std::vector<std::string> split(const std::string &s, char delim)
    {
        std::vector<std::string> tokens;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, delim))
            tokens.push_back(tok);
        return tokens;
    }
};
