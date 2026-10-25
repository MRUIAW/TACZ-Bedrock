#include "pack/LenientJson.h"
#include "pack/GunPackParser.h"
#include <algorithm>
#include <cctype>

namespace tacz::pack {
namespace {
class Reader {
    std::string_view text;
    size_t at=0;
    [[noreturn]] void error(char const* message) const {
        throw ParseError("lenient JSON byte " + std::to_string(at) + ": " + message);
    }
    void space() {
        for (;;) {
            while (at<text.size() && std::string_view(" \t\r\n\f").find(text[at])!=std::string_view::npos) ++at;
            if (at==text.size()) return;
            if (text[at]=='#' || text.substr(at,2)=="//") {
                while (at<text.size() && text[at]!='\n' && text[at]!='\r') ++at;
            } else if (text.substr(at,2)=="/*") {
                auto end=text.find("*/",at+2);
                if (end==std::string_view::npos) error("unterminated comment");
                at=end+2;
            } else return;
        }
    }
    bool take(char c) { space(); if (at<text.size() && text[at]==c) { ++at; return true; } return false; }
    std::string quoted() {
        char quote=text[at++];
        std::string normalized="\"";
        while (at<text.size()) {
            char c=text[at++];
            if (c==quote) {
                normalized+='"';
                try { return nlohmann::json::parse(normalized).get<std::string>(); }
                catch (nlohmann::json::exception const&) { error("invalid string escape/UTF-8"); }
            }
            if (c=='\\') {
                if (at==text.size()) error("unterminated escape");
                char e=text[at++];
                if (e=='\'') normalized+='\'';
                else if (e=='\n') normalized+="\\n";
                else if (std::string_view("\"\\/bfnrtu").find(e)!=std::string_view::npos) { normalized+='\\'; normalized+=e; }
                else error("invalid escape");
            } else if (c=='"') normalized+="\\\"";
            else if (static_cast<unsigned char>(c)<32) {
                constexpr char hex[]="0123456789abcdef";
                normalized+="\\u00"; normalized+=hex[(static_cast<unsigned char>(c)>>4)&15]; normalized+=hex[c&15];
            } else normalized+=c;
        }
        error("unterminated string");
    }
    std::string literal() {
        size_t start=at;
        while (at<text.size() && std::string_view("{}[],:;=\\/# \t\f\r\n").find(text[at])==std::string_view::npos) ++at;
        if (at==start) error("expected literal");
        return std::string(text.substr(start,at-start));
    }
    nlohmann::json value(int depth) {
        if (depth>512) error("nesting limit");
        space();
        if (at==text.size()) error("expected value");
        if (text[at]=='"' || text[at]=='\'') return quoted();
        if (take('{')) {
            auto out=nlohmann::json::object();
            if (take('}')) return out;
            for (;;) {
                space();
                if (at==text.size() || text[at]=='}') error("expected object name (trailing object comma is invalid)");
                auto name=(text[at]=='"' || text[at]=='\'') ? quoted() : literal();
                if (!take(':')) { if (!take('=')) error("expected ':'"); if (at<text.size() && text[at]=='>') ++at; }
                out[name]=value(depth+1);
                if (take('}')) return out;
                if (!take(',') && !take(';')) error("expected object separator");
            }
        }
        if (take('[')) {
            auto out=nlohmann::json::array();
            if (take(']')) return out;
            for (;;) {
                space();
                if (at<text.size() && (text[at]==',' || text[at]==';' || text[at]==']')) out.push_back(nullptr);
                else out.push_back(value(depth+1));
                if (take(']')) return out;
                if (!take(',') && !take(';')) error("expected array separator");
            }
        }
        auto token=literal(), lower=token;
        std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower=="null") return nullptr;
        if (lower=="true") return true;
        if (lower=="false") return false;
        auto number=nlohmann::json::parse(token,nullptr,false);
        if (number.is_number()) return number;
        // +0.15, 01, NaN etc. are unquoted STRING literals in Gson's tree model.
        return token;
    }
public:
    explicit Reader(std::string_view input):text(input) {}
    nlohmann::json read() {
        if (text.starts_with("\xEF\xBB\xBF")) at=3;
        space();
        if (text.substr(at,5)==")]}'\n") at+=5;
        space();
        if (at==text.size()) return nullptr; // Gson empty document
        auto result=value(0);
        space();
        if (at!=text.size()) error("trailing content");
        return result;
    }
};
}
nlohmann::json readLenientJson(std::string_view text) { return Reader(text).read(); }
}
