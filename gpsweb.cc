/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "base64.h"
#include "gpsweb.h"
#include "gpstracker.h"
#include "messagebus.h"

// Don't bind to the http server until we're created, so pass a null to
// the stream_handler init
kis_gps_web::kis_gps_web(shared_gps_builder in_builder) : 
    kis_gps(in_builder) {

    last_heading_time = 0;

    auto httpd = Globalreg::fetch_mandatory_global_as<kis_net_beast_httpd>();

    httpd->register_route("/gps/web/update", {"POST"}, httpd->LOGON_ROLE, {"cmd"},
            std::make_shared<kis_net_web_function_endpoint>(
                [this](std::shared_ptr<kis_net_beast_httpd_connection> con) {
                    std::ostream stream(&con->response_stream());

                    double lat = 0, lon = 0, alt = 0, spd = 0;
                    bool set_alt = false, set_spd = false;

                    lat = con->json()["lat"].asDouble();
                    lon = con->json()["lon"].asDouble();
                    
                    if (!con->json()["alt"].isNull()) {
                        alt = con->json()["alt"].asDouble();
                        set_alt = true;
                    }

                    if (!con->json()["spd"].isNull()) {
                        spd = con->json()["spd"].asDouble();
                        set_spd = true;
                    }

                    auto new_location = packetchain->new_packet_component<kis_gps_packinfo>();
                    new_location->lat = lat;
                    new_location->lon = lon;
                    new_location->fix = 2;

                    if (set_alt) {
                        new_location->alt = alt;
                        new_location->fix = 3;
                    }

                    if (set_spd) 
                        new_location->speed = spd;

                    gettimeofday(&(new_location->tv), NULL);
                    new_location->gpsuuid = get_gps_uuid();
                    new_location->gpsname = get_gps_name();

                    if (time(0) - last_heading_time > 5 &&
                        gps_location != nullptr && gps_location->fix >= 2) {
                        new_location->heading = 
                            gps_calc_heading(new_location->lat, new_location->lon, 
                                             gps_location->lat, gps_location->lon);
                        last_heading_time = new_location->tv.tv_sec;
                    }

                    gps_last_location = gps_location;
                    gps_location = new_location;

                    // Sync w/ the tracked fields
                    update_locations();

                    stream << "Updated\n";
                }));

    httpd->register_websocket_route("/gps/web/update", {httpd->LOGON_ROLE, "WEBGPS"}, {"ws"},
            std::make_shared<kis_net_web_function_endpoint>(
                [this](std::shared_ptr<kis_net_beast_httpd_connection> con) {
                    auto ws = 
                        std::make_shared<kis_net_web_websocket_endpoint>(con,
                                [this](std::shared_ptr<kis_net_web_websocket_endpoint> ws,
                                    boost::beast::flat_buffer& buf, bool text) {

                                    if (!text) {
                                        ws->close();
                                        return;
                                    }

                                    std::stringstream stream;

                                    std::stringstream ss(boost::beast::buffers_to_string(buf.data()));
                                    Json::Value json;

                                    try {
                                        ss >> json;

                                        double lat = 0, lon = 0, alt = 0, spd = 0;
                                        bool set_alt = false, set_spd = false;

                                        lat = json["lat"].asDouble();
                                        lon = json["lon"].asDouble();

                                        if (!json["alt"].isNull()) {
                                            alt = json["alt"].asDouble();
                                            set_alt = true;
                                        }

                                        if (json["spd"].isNull()) {
                                            spd = json["spd"].asDouble();
                                            set_spd = true;
                                        }

                                        auto new_location = 
                                            packetchain->new_packet_component<kis_gps_packinfo>();
                                        new_location->lat = lat;
                                        new_location->lon = lon;
                                        new_location->fix = 2;

                                        if (set_alt) {
                                            new_location->alt = alt;
                                            new_location->fix = 3;
                                        }

                                        if (set_spd) 
                                            new_location->speed = spd;

                                        gettimeofday(&(new_location->tv), NULL);
                                        new_location->gpsuuid = get_gps_uuid();
                                        new_location->gpsname = get_gps_name();

                                        if (time(0) - last_heading_time > 5 &&
                                                gps_location != nullptr && gps_location->fix >= 2) {
                                            new_location->heading = 
                                                gps_calc_heading(new_location->lat, new_location->lon, 
                                                        gps_location->lat, gps_location->lon);
                                            last_heading_time = new_location->tv.tv_sec;
                                        }

                                        gps_last_location = gps_location;
                                        gps_location = new_location;

                                        // Sync w/ the tracked fields
                                        update_locations();

                                        stream << "{\"update\": \"ok\"}";

                                        ws->write(stream.str(), true);
                                    } catch (const std::exception& e) {
                                        _MSG_ERROR("Invalid websocket request (could not parse JSON message) on "
                                                "/gps/web/update.ws");
                                        stream << "{\"update\": \"error\"}";
                                        ws->write(stream.str(), true);
                                        return;
                                    }
                                });
                }
                ));

}

kis_gps_web::~kis_gps_web() {

}

bool kis_gps_web::open_gps(std::string in_opts) {
    kis_lock_guard<kis_mutex> lk(gps_mutex, "gps_web open_gps");

    if (!kis_gps::open_gps(in_opts)) {
        return false;
    }

    set_int_gps_description("web-based GPS using location from browser");

    return true;
}

bool kis_gps_web::get_location_valid() {
    kis_lock_guard<kis_mutex> lk(gps_mutex, "gps_web get_location_valid");

    if (gps_location == NULL) {
        return false;
    }

    if (gps_location->fix < 2) {
        return false;
    }

    // Allow a wider location window
    if (time(0) - gps_location->tv.tv_sec > 30) {
        return false;
    }

    return true;
}

bool kis_gps_web::get_device_connected() {
    if (gps_location == NULL)
        return false;

    // If we've seen a GPS update w/in the past 2 minutes, we count as 'connected' to a gps
    if (time(0) - gps_location->tv.tv_sec > 120) {
        return false;
    }

    return true;
}

