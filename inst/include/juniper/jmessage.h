#ifndef juniper_juniper_jmessage_H
#define juniper_juniper_jmessage_H

#include <fstream>
#include <string>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <json.hpp>
#include <hmac.h>
#include <ctime>
#include <iostream>
#include <sha256.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <juniper/external.h>
#include <Rcpp.h>

#define VERSION "5.2"
static const std::string DELIMITER = "<IDS|MSG>";

using nlohmann::json;
// Here's where we read and write messages using the jupyter protocol.
// Since the protocol is actually serialized python dicts (a.k.a. JSON),
// we are stuck with json (de)serialization.
class JMessage {
  
public:
  std::string _key; // used for creating the hmac signature
  static JMessage read(zmq::multipart_t& request, const std::string& key) {
    JMessage jm;
    jm._key = key;
    return jm.read_ids(request).read_hmac(request).read_body(request);
  }

  static zmq::multipart_t reply(const JMessage& parent, const std::string& msg_type, const json& content) {
    JMessage jm;
    jm._key = parent._key;
    // construct header
    jm._msg["header"]["msg_id"] = uuid();
    jm._msg["header"]["username"] = parent._msg["header"]["username"];
    jm._msg["header"]["session"] = parent._msg["header"]["session"];
    jm._msg["header"]["date"] = now();
    jm._msg["header"]["msg_type"] = msg_type;
    jm._msg["header"]["version"] = VERSION;

    jm._msg["parent_header"] = parent._msg["header"];
    jm._ids = parent._ids;
    jm._msg["metadata"] = json({});
    jm._msg["content"] = content;
    return jm.to_multipart_t();
  }

  json get() const { return _msg; }

private:

  json _msg;
  std::string _hmac;
  std::vector<std::string> _ids;
  
  bool not_delimiter(const zmq::message_t& m, std::string& id) {
    id=read_str(m);
    return id.compare(DELIMITER)!=0;
  }

  JMessage& read_ids(zmq::multipart_t& msg) {
    zmq::message_t fr = msg.pop();
    std::string id;
    while( fr.size()!=0 && not_delimiter(fr, id) ) {
      _ids.emplace_back(id);
      fr=msg.pop();
    }
    return *this;
  }
  
  JMessage& read_hmac(zmq::multipart_t& msg) {
    _hmac = read_str(msg.pop());
    return *this;
  }
  
  JMessage& read_body(zmq::multipart_t& msg) {
    std::stringstream data;
    _msg["header"]        = read_json(msg.pop(), data);
    _msg["parent_header"] = read_json(msg.pop(), data);
    _msg["metadata"]      = read_json(msg.pop(), data);
    _msg["content"]       = read_json(msg.pop(), data);

    // validate
    std::string hmac2dig = hmac<SHA256>(data.str(), _key);
    // Rcpp::Rcout << "hmac2dig: " << hmac2dig << "; actual: " << _hmac << std::endl;
    // Rcpp::Rcout << "hmac2dig.compare(actual)= " << hmac2dig.compare(_hmac) << std::endl;
    if( hmac2dig!=_hmac )
      throw("bad hmac digest");
    return *this;
  }

  // ideally read the message one time; for validation we want to cat together
  // the header, parent header, metadata, and content dicts; at the same time
  // as we read the fields of this struct, we also build up the cat'd string
  // with the data stringstream.
  static json read_json(const zmq::message_t& msg, std::stringstream& data) {
    std::string s = read_str(msg);
    data << s;
    return json::parse(s);
  }

  static std::string read_str(const zmq::message_t& msg) {
    std::stringstream ss;
    const char* buf = msg.data<const char>();
    size_t buflen = msg.size();
    for(size_t i=0; i<buflen; ++i)
      ss << static_cast<char>(buf[i]);
    return ss.str();
  }

  // boost headers available from R package BH
  static std::string uuid() {
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    std::stringstream s; s << uuid;
    return s.str();
  }

  // from https://stackoverflow.com/questions/9527960/how-do-i-construct-an-iso-8601-datetime-in-c
  static std::string now() {
    time_t now;
    time(&now);
    char buf[sizeof "2011-10-08T07:07:09Z"];
    strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
    // this will work too, if your compiler doesn't support %F or %T:
    //strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    std::stringstream s;
    s << buf;
    return s.str();
  }
  
  static zmq::message_t to_msg(const std::string& s) {
    return zmq::message_t(s.begin(), s.end());
  }

  // JMessage (this) -> multipart_t
  zmq::multipart_t to_multipart_t() {
    std::stringstream s;
    zmq::multipart_t multi_msg;
    
    // add IDS
    for(std::string id: _ids) {
      multi_msg.add(to_msg(id));
    }

    // now the delimiter
    multi_msg.add(to_msg(DELIMITER));
    
    // construct the hmac sig
    std::stringstream data;
    std::string header        = _msg["header"       ].dump(); data << header       ;
    std::string parent_header = _msg["parent_header"].dump(); data << parent_header;
    std::string metadata      = _msg["metadata"     ].dump(); data << metadata     ;
    std::string content       = _msg["content"      ].dump(); data << content      ;

    // serialize the signature and main message body
    std::string hmacsig = hmac<SHA256>(data.str(), _key);
    multi_msg.add(to_msg(hmacsig));
    multi_msg.add(to_msg(header));
    multi_msg.add(to_msg(parent_header));
    multi_msg.add(to_msg(metadata));
    multi_msg.add(to_msg(content));
    return multi_msg;
  }
};

#endif // ifndef juniper_juniper_jmessage_H
