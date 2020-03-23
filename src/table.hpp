#ifndef _ROUTINGTABLE_
#define _ROUTINGTABLE_

#include <map>
#include <string>
#include <vector>
#include <set>
#include <iostream>

typedef std::string routerId;                     // "ip_addr"
typedef std::pair<routerId, double> linkWeight;   // ("src", "weight")
typedef std::map<routerId, linkWeight> costTable; // {"dest" -> ("src", "weight"),...}
typedef std::vector<linkWeight> linkList;         // [("src", "weight"),...]
typedef std::map<routerId, linkList> routerGraph; // {"src" -> [("src", "weight"),...], ...}

class RoutingTable {
public:
    RoutingTable(const routerId& id) : _localRouter(id) {}

    void updateMyLink(const routerId& dest, double weight) {
        if (_graph.count(_localRouter) == 0 && weight != 0) {
            // add first link
            linkList links;
            links.push_back(linkWeight(dest, weight));
            _graph[_localRouter] = links;
            _updateCostTable();
            return;
        }
        for (auto i = _graph[_localRouter].begin(); i != _graph[_localRouter].end(); ++i) {
            if (dest == i->first) {
                if (weight == 0) {
                    // delete
                    _graph[_localRouter].erase(i);
                    _updateCostTable();
                    return;
                }
                // update
                i->second = weight;
                _updateCostTable();
                return;
            }
        }
        if (weight != 0) {
            // add later link
            _graph[_localRouter].push_back(linkWeight(dest, weight));
            _updateCostTable();
        }
    }

    void updateLinks(const routerId& r, const linkList& links) {
        _graph[r] = links;
        _updateCostTable();
    }

    routerId getThis(){return _localRouter;}

    routerId nextHop(routerId dest) {
        auto i = _costs.find(dest);
        while (i != _costs.end() && _localRouter != i->second.first) {
            dest = i->second.first;
            i = _costs.find(dest);
        }
        if (i != _costs.end()) {
            return dest;
        }
        return routerId();
    }

    const std::string printRoute(routerId dest) {
        std::list<linkWeight> path;
        auto i = _costs.find(dest);
        while (i != _costs.cend() && _localRouter != i->second.first) {
            path.push_front(linkWeight(dest, i->second.second));
            dest = i->second.first;
            i = _costs.find(dest);
        }
        if (i != _costs.end()) {
            double pathCost = 0.0;
            path.push_front(linkWeight(dest, i->second.second));
            std::ostringstream oss;
            routerId src = _localRouter;
            for (auto s = path.cbegin(); s != path.cend(); ++s) {
                oss << src << " --> " << s->first << "\t" << s->second - pathCost << std::endl;
                src = s->first;
                pathCost += s->second - pathCost;
            }
            oss << "Total cost: " << pathCost << std::endl;
            return oss.str();
        }
        return "No path to destination.\n";
    }

    const std::string printGraph() const {
        std::ostringstream oss;
        oss << "Source\t\tDestination\tCost\n";
        oss << "------\t\t-----------\t----\n";
        for (auto i = _graph.cbegin(); i != _graph.cend(); ++i) {
            for (auto j = i->second.cbegin(); j != i->second.cend(); ++j) {
                oss << i->first << "\t" << j->first << "\t" << j->second << std::endl;
            }
        }
        return oss.str();
    }

private:
    void _updateCostTable() {
        std::set<std::string> visitedNodes;

        // init
        _costs.clear();
        _costs[_localRouter] = linkWeight("", 0.0);
        for (auto i = _graph[_localRouter].cbegin(); i != _graph[_localRouter].cend(); ++i) {
            _costs[i->first] = linkWeight(_localRouter, i->second);
        }

        // search
        while (visitedNodes.size() < _costs.size()) {
            // visit the smallest cost un-visited node remaining
            routerId min_r;
            double min_cost = -1.0;
            for (auto x = _graph.cbegin(); x != _graph.cend(); ++x) {
                if (visitedNodes.count(x->first)) {
                    continue;
                }
                if (_costs.count(x->first) &&
                    (min_cost < 0 || _costs[x->first].second < min_cost))
                {
                    min_r = x->first;
                    min_cost = _costs[x->first].second;
                }
            }
            if (min_cost < 0) {
                break;
            }

            visitedNodes.insert(min_r);

            // update costs in forwarding table
            for (auto x = _graph[min_r].cbegin(); x != _graph[min_r].cend(); ++x) {
                    if (visitedNodes.count(x->first)) {
                        continue;
                    }
                    if (_costs.count(x->first)) {
                        if (_costs[x->first].second > min_cost + x->second) {
                            linkWeight l(min_r, min_cost + x->second);
                            _costs[x->first] = l;
                        }
                    } else {
                        linkWeight l(min_r, min_cost + x->second);
                        _costs[x->first] = l;
                    }
            }
        }
    }

    routerId _localRouter;
    routerGraph _graph;
    costTable _costs;
};

#endif // _ROUTINGTABLE_