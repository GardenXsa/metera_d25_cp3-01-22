#include <iostream>
#include <sstream>
#include <string>

class JsonWriter {
private:
    std::ostringstream ss;
    int indent_level = 0;
    
    void write_indent() {
        for (int i = 0; i < indent_level; i++) ss << "  ";
    }
    
public:
    void startObject() { ss << "{"; indent_level++; }
    void endObject() { indent_level--; ss << "}"; }
    
    void key(const std::string& k) {
        if (indent_level > 0) write_indent();
        ss << "\"" << k << "\": ";
    }
    
    void value(const std::string& v) {
        ss << "\"" << v << "\"";
    }
    
    void comma() { ss << ","; }
    
    std::string str() const { return ss.str(); }
};

std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    
    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) pos++;
    
    if (pos >= json.length()) return "";
    
    if (json[pos] != '"') {
        size_t end = pos;
        while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != ']' && json[end] != ' ') {
            end++;
        }
        return json.substr(pos, end - pos);
    }
    
    pos++;
    std::string result;
    while (pos < json.length()) {
        if (json[pos] == '\\' && pos + 1 < json.length()) {
            pos++;
            switch (json[pos]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += json[pos]; break;
            }
            pos++;
        } else if (json[pos] == '"') {
            break;
        } else {
            result += json[pos++];
        }
    }
    
    return result;
}

int main() {
    // Тест записи
    JsonWriter w;
    w.startObject();
    w.key("status"); w.value("ok");
    w.comma();
    w.key("message"); w.value("Engine initialized");
    w.endObject();
    std::string output = w.str();
    std::cout << "OUTPUT: " << output << std::endl;
    
    // Тест чтения
    std::string status = extractJsonString(output, "status");
    std::string message = extractJsonString(output, "message");
    std::cout << "status='" << status << "'" << std::endl;
    std::cout << "message='" << message << "'" << std::endl;
    
    return 0;
}
