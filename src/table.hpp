#ifndef _ROUTINGTABLE_
#define _ROUTINGTABLE_

#include <map>
#include <string>
#include <vector>
#include <set>

typedef std::string routerId;                     // "ip_addr"
typedef std::pair<routerId, double> linkWeight;   // ("src", "weight")
typedef std::map<routerId, linkWeight> costTable; // {"dest" -> ("src", "weight"),...}
typedef std::vector<linkWeight> linkList;         // [("src", "weight"),...]
typedef std::map<routerId, linkList> routerGraph; // {"src" -> [("src", "weight"),...], ...}

class RoutingTable {
public:
    RoutingTable(const routerId& id) : _localRouter(id) {}

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
        if (_costs.find(dest) != _costs.end()) {
            return dest;
        }
        return routerId();
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