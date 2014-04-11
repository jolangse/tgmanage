// Find optimal assignment of access switches using min-cost max-flow.
// About three times as fast as the old DP+heuristics-based solution
// (<2ms for planning TG), and can deal with less regular cost metrics.
//
// Given D distro switches and N access switches, complexity is approx. O(n²(log d+log n))
// (runs n iterations, each iteration is O(Vlog E), V is O(n), E is O(dn))).
//
// g++ -std=gnu++11 -Wall -g -O3 -fopenmp -DOUTPUT_FILES=1 -o planning planning.cpp && ./planning -3 -6 11 22 -26 35

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <atomic>
#include <vector>
#include <map>
#include <string>
#include <queue>

#define NUM_DISTRO 6
#define NUM_ROWS 41
#define SWITCHES_PER_ROW 4
#define PORTS_PER_DISTRO 38

#define TRUNCATE_METRIC 1
#define EXTENSION_COST 70
#define HORIZ_GAP_COST 100

#define FIRST_SUBNET_ADDRESS "151.216.1.0"
#define SUBNET_SIZE 26

#define _INF 99999

using namespace std;

struct Switch {
	unsigned row, num;

	Switch(unsigned row, unsigned num) : row(row), num(num) {}
};

struct Inventory {
	Inventory() : num_10m(0), num_30m(0), num_50m(0), extensions(0), horiz_gap_crossings(0), vert_chasm_crossings(0) {}

	Inventory& operator+= (const Inventory& other)
	{
		this->num_10m += other.num_10m;
		this->num_30m += other.num_30m;
		this->num_50m += other.num_50m;
		this->extensions += other.extensions;
		this->horiz_gap_crossings += other.horiz_gap_crossings;
		this->vert_chasm_crossings += other.vert_chasm_crossings;
		return *this;
	}

	string to_string() const
	{
		if (num_10m >= _INF) {
			return "XXXXX";
		}

		string ret;
		Inventory copy = *this;
		while (copy.num_50m-- > 0) {
			if (!ret.empty()) {
				ret += '+';
			}
			ret += "50";
		}
		while (copy.num_30m-- > 0) {
			if (!ret.empty()) {
				ret += '+';
			}
			ret += "30";
		}
		while (copy.num_10m-- > 0) {
			if (!ret.empty()) {
				ret += '+';
			}
			ret += "10";
		}
		return ret;
	}

	unsigned num_10m, num_30m, num_50m;
	unsigned extensions, horiz_gap_crossings, vert_chasm_crossings;
};

// Data structures for flow algorithm.
struct Node;
struct Edge {
	Node *to;
	Edge *reverse;  // Edge in opposite direction.

	int capacity, flow;
	int cost;
};
struct Node {
	vector<Edge *> edges;

	// For debugging.
	char name[16];

	// Used in Dijkstra search.
	int cost_from_source;
	bool seen;
	Edge *prev_edge;
};
struct Graph {
	Node source_node, sink_node;
	Node distro_nodes[NUM_DISTRO];
	vector<Node> switch_nodes;
	vector<Edge> edges;
	vector<Node*> all_nodes;
};

const unsigned horiz_cost[SWITCHES_PER_ROW] = {
	216, 72, 72, 216  // Gap costs are added separately.
};

struct VerticalGap {
	unsigned after_row_num;
	unsigned extra_cost;
};
// 3, 4m, 4m, 4m gaps (0.6m, 1.6m, 1.6m, 1.6m extra).
vector<VerticalGap> vertical_gaps = {
	{ 5, 6 },
	{ 13, 16 },
	{ 21, 16 },
	{ 30, 16 },
};

class Planner {
 private:
	int distro_placements[NUM_DISTRO];
	vector<Switch> switches;
	map<unsigned, unsigned> num_ports_used;
	string *log_buf;

	unsigned find_distance(Switch from_where, int distro);
	unsigned find_cost(Switch from_where, int distro);
	Inventory find_inventory(Switch from_where, int distro);
	unsigned find_slack(Inventory inventory, unsigned distance);
	void logprintf(const char *str, ...);
	void init_switches();
	void construct_graph(const vector<Switch> &switches, Graph *g);
	void find_mincost_maxflow(Graph *g);
	void print_switch(const Graph &g, int i, int distro);

 public:
	Planner() : log_buf(NULL) {}
	void set_log_buf(string *log_buf) { this->log_buf = log_buf; }
	int do_work(int distro_placements[NUM_DISTRO]);
};

unsigned Planner::find_distance(Switch from_where, int distro)
{
	assert(distro != -1);
	const unsigned dp = std::abs(distro_placements[distro]);

	unsigned base_cost = horiz_cost[from_where.num];

	if ((distro_placements[distro] >= 0) == (from_where.num >= 2)) {
		int bridge_row = distro_placements[NUM_DISTRO - 1];

		// Go to the bridge...
		base_cost += 36 * abs(int(from_where.row) - bridge_row);

		// Cross it (5.0m horizontal gap)...
		base_cost += 50;

		// ...and away from the bridge again.
		base_cost += 36 * abs(int(dp) - bridge_row);
	} else {
		// 3.6m from row to row (2.4m gap + 1.2m boards).
		base_cost += 36 * abs(int(from_where.row) - int(dp));
	}

	for (const VerticalGap& gap : vertical_gaps) {
		if ((from_where.row <= gap.after_row_num) == (dp > gap.after_row_num)) {
			base_cost += gap.extra_cost;
		}
	}

	// Add 5m slack.
	return base_cost + 50;
}
	
Inventory Planner::find_inventory(Switch from_where, int distro)
{
	assert(distro != -1);

	unsigned distance = find_distance(from_where, distro);
	Inventory inv;
	if (distance <= 100) {
		inv.num_10m = 1;
	} else if (distance <= 200) {
		inv.num_10m = 2;
		inv.extensions = 1;
	} else if (distance <= 300) {
		inv.num_30m = 1;
	} else if (distance <= 400) {
		inv.num_10m = 1;
		inv.num_30m = 1;
		inv.extensions = 1;
	} else if (distance <= 500) {
		inv.num_50m = 1;
	} else if (distance <= 600) {
		inv.num_10m = 1;
		inv.num_50m = 1;
		inv.extensions = 1;
	} else if (distance <= 800) {
		inv.num_30m = 1;
		inv.num_50m = 1;
		inv.extensions = 1;
	} else if (distance <= 1000) {
		inv.num_50m = 2;
		inv.extensions = 1;
	} else {
		inv.num_10m = _INF;
	}

	if ((distro_placements[distro] >= 0) == (from_where.num >= 2)) {
		inv.horiz_gap_crossings = 1;
	}

	// The gap between Game and Sector 8 is unsurmountable.
	if ((abs(distro_placements[distro]) <= 5) == (from_where.row >= 6)) {
		inv.vert_chasm_crossings = 1;
	}

	// So is the gap over the scene.
	if ((abs(distro_placements[distro]) <= 13) == (from_where.row >= 14) &&
	    from_where.num >= 2 && distro_placements[distro] < 0) {
		inv.vert_chasm_crossings = 1;
	}

	return inv;
}

unsigned Planner::find_slack(Inventory inventory, unsigned distance)
{
	return 100 * inventory.num_10m + 300 * inventory.num_30m + 500 * inventory.num_50m - distance;
}

unsigned Planner::find_cost(Switch from_where, int distro)
{
	Inventory inv = find_inventory(from_where, distro);
	unsigned cost;

#if TRUNCATE_METRIC
	cost = 100 * inv.num_10m + 300 * inv.num_30m + 500 * inv.num_50m + EXTENSION_COST * inv.extensions;
	// cost = find_slack(inv, distance);
#else
	cost = find_distance(from_where, distro);
	// cost = ((distance + 90) / 100) * 100;
#endif

#if 0
	// We really, really do not want to cross the gap on the north side.
	// (now handled in bridge cost)
	if (from_where.row <= 30) {
		cost += _INF * inv.horiz_gap_crossings;
	} else {
		cost += HORIZ_GAP_COST * inv.horiz_gap_crossings;
	}
#endif

	// Also, the gap between Game and Sector 8 is unsurmountable.
	cost += _INF * inv.vert_chasm_crossings;

	return cost;
}

void Planner::logprintf(const char *fmt, ...)
{
	if (log_buf == NULL) {
		return;
	}

	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	log_buf->append(buf);
}

string distro_name(unsigned distro)
{
	char buf[16];
	sprintf(buf, "distro%d", distro + 1);
	return buf;
}

string port_name(unsigned distro, unsigned portnum)
{
	char buf[16];
	int distros[] = { 1, 2, 5, 6 };
	sprintf(buf, "Gi%u/%u", distros[portnum / 48], (portnum % 48) + 1);
	return buf;
}

void Planner::init_switches()
{
	switches.clear();
	for (unsigned i = 1; i <= NUM_ROWS; ++i) {
		// Game area.
		if (i >= 1 && i <= 5) {
			switches.push_back(Switch(i, 2));
			switches.push_back(Switch(i, 3));
		}

		// Sectors 7 and 8.
		if (i >= 6 && i <= 13) {
			switches.push_back(Switch(i, 0));
			switches.push_back(Switch(i, 1));
			switches.push_back(Switch(i, 2));
			switches.push_back(Switch(i, 3));
		}

		// Sector 5.
		if (i >= 14 && i <= 21) {
			switches.push_back(Switch(i, 0));
			switches.push_back(Switch(i, 1));
		}

		// Sectors 3 and 4.
		if (i >= 22 && i <= 30) {
			switches.push_back(Switch(i, 0));
			switches.push_back(Switch(i, 1));
			switches.push_back(Switch(i, 2));
			switches.push_back(Switch(i, 3));
		}

		// Sector 1.
		if (i >= 31 && i <= 41) {
			switches.push_back(Switch(i, 0));
			switches.push_back(Switch(i, 1));
		}

		// Sector 2.
		if (i >= 31 && i <= 39) {
			switches.push_back(Switch(i, 2));
			switches.push_back(Switch(i, 3));
		}
	}
}

void add_edge(Node *from, Node *to, int capacity, int cost, vector<Edge> *edges)
{
	assert(edges->size() + 2 <= edges->capacity());
	edges->resize(edges->size() + 2);

	Edge *e1 = &edges->at(edges->size() - 2);
	Edge *e2 = &edges->at(edges->size() - 1);

	e1->to = to;
	e1->capacity = capacity;
	e1->flow = 0;
	e1->cost = cost;
	e1->reverse = e2;
	from->edges.push_back(e1);

	e2->to = from;
	e2->capacity = 0;
	e2->flow = 0;
	e2->cost = -cost;
	e2->reverse = e1;
	to->edges.push_back(e2);
}

void Planner::construct_graph(const vector<Switch> &switches, Graph *g)
{
	// Min-cost max-flow in a graph that looks something like this
	// (ie., all distros connect to all access switches):
	//
	//         ---- D1 \---/-- A1 --
	//        /         \ /         \          .
	// source ----- D2 --X---- A2 --- sink
	//        \         / \         /
 	//         ---- D3 /---\-- A3 -/
	//
	// Capacity from source to distro is 48 (or whatever), cost is 0.
	// Capacity from distro to access is 1, cost is cable length + penalties.
	// Capacity from access to sink is 1, cost is 0.
	g->switch_nodes.resize(switches.size());
	g->edges.reserve(switches.size() * NUM_DISTRO * 2 + 16);

	for (unsigned i = 0; i < NUM_DISTRO; ++i) {
		add_edge(&g->source_node, &g->distro_nodes[i], PORTS_PER_DISTRO, 0, &g->edges);
	}
	for (unsigned i = 0; i < NUM_DISTRO; ++i) {
		for (unsigned j = 0; j < switches.size(); ++j) {
			int cost = find_cost(switches[j], i);
			if (cost >= _INF) {
				continue;
			}
			add_edge(&g->distro_nodes[i], &g->switch_nodes[j], 1, cost, &g->edges);
		}
	}
	for (unsigned i = 0; i < switches.size(); ++i) {
		add_edge(&g->switch_nodes[i], &g->sink_node, 1, 0, &g->edges);
	}
	
	g->all_nodes.push_back(&g->source_node);
	strcpy(g->source_node.name, "source");

	g->all_nodes.push_back(&g->sink_node);
	strcpy(g->sink_node.name, "sink");

	for (unsigned i = 0; i < NUM_DISTRO; ++i) {
		g->all_nodes.push_back(&g->distro_nodes[i]);
		sprintf(g->distro_nodes[i].name, "distro%d", i);
	}
	for (unsigned i = 0; i < switches.size(); ++i) {
		g->all_nodes.push_back(&g->switch_nodes[i]);
		sprintf(g->switch_nodes[i].name, "switch%d", i);
	}
}

void Planner::find_mincost_maxflow(Graph *g)
{
	// We use the successive shortest path algorithm, using a simple
	// heap-based Dijkstra (O(Vlog E)) for search.
	int num_paths = 0;
	for ( ;; ) {
		// Reset Dijkstra state.
		for (Node *n : g->all_nodes) {
			n->cost_from_source = _INF;
			n->seen = false;
			n->prev_edge = NULL;
		}
		g->source_node.cost_from_source = 0;

		priority_queue<pair<int, Node*>> q;
		q.push(make_pair(0, &g->source_node));

		while (!q.empty()) {
			Node *u = q.top().second;
			q.pop();
			if (u->seen) {
				continue;
			}
			u->seen = true;
			if (u == &g->sink_node) {
				// Yay, we found a path to the sink.
				break;
			}

			// Relax outgoing edges from this node.
			for (Edge *e : u->edges) {
				Node *v = e->to;
				if (v->seen) {
					continue;
				}
				if (e->flow + 1 > e->capacity || e->reverse->flow - 1 > e->reverse->capacity) {
					// Not feasible.
					continue;
				}
				if (v->cost_from_source <= u->cost_from_source + e->cost) {
					// Already seen through a better path.
					continue;
				}
				v->prev_edge = e;
				v->cost_from_source = u->cost_from_source + e->cost;
				q.push(make_pair(-v->cost_from_source, v));
			}
		}
		if (q.empty()) {
			// Oops, no usable path.
			goto end;
		}

		// Increase flow along the path, moving backwards towards the source.
		Node *n = &g->sink_node;
		for ( ;; ) {
			if (n->prev_edge == NULL) {
				break;
			}

			n->prev_edge->flow += 1;
			n->prev_edge->reverse->flow -= 1;

			n = n->prev_edge->reverse->to;
		}
		++num_paths;
	}

end:
	logprintf("Augmented using %d paths.\n", num_paths);
}

// Figure out which distro each switch was connected to.
map<int, int> find_switch_distro_map(const Graph &g)
{
	map<int, int> ret;
	for (unsigned distro_num = 0; distro_num < NUM_DISTRO; ++distro_num) {
		for (Edge *e : g.distro_nodes[distro_num].edges) {
			if (e->flow <= 0) {
				continue;
			}
			if (e->to >= &g.switch_nodes[0] && e->to < &g.switch_nodes[g.switch_nodes.size()]) {
				int switch_index = (e->to - &g.switch_nodes[0]);
				ret.insert(make_pair(switch_index, distro_num));
			}
		}
	}
	return ret;
}

void Planner::print_switch(const Graph &g, int i, int distro)
{
	if (i == -1) {
		logprintf("%16s", "");
		return;
	}
	if (distro == -1) {
		logprintf("[%u;22m- ", distro + 32);
#if TRUNCATE_METRIC
		logprintf("(XXXXX) (XXXX)");
#else
		logprintf("(XXXX)");
#endif
	} else {
		logprintf("[%u;22m%u ", distro + 32, distro);

		int this_distance = find_distance(switches[i], distro);
#if TRUNCATE_METRIC
		Inventory this_inv = find_inventory(switches[i], distro);
		logprintf("(%-5s) (%3.1f)", this_inv.to_string().c_str(), this_distance / 10.0);
#else
		logprintf("(%3.1f)", this_distance / 10.0);
#endif
	}

}

int Planner::do_work(int distro_placements[NUM_DISTRO])
{
	memcpy(this->distro_placements, distro_placements, sizeof(distro_placements[0]) * NUM_DISTRO);

	num_ports_used.clear();

	Inventory total_inv;
	unsigned total_cost = 0, total_slack = 0;

	init_switches();

	logprintf("Finding optimal layout for %u switches\n", switches.size());

	Graph g;
	construct_graph(switches, &g);
	find_mincost_maxflow(&g);
	map<int, int> switches_to_distros = find_switch_distro_map(g);

	for (unsigned row = 1; row <= NUM_ROWS; ++row) {
		// Figure out distro markers.
		char distro_marker_left[16] = " ";
		char distro_marker_right[16] = " ";
		for (int d = 0; d < NUM_DISTRO; ++d) {
			if (int(row) == distro_placements[d]) {
				sprintf(distro_marker_left, "[%u;1m*", d + 32);
			}
			if (int(row) == -distro_placements[d]) {
				sprintf(distro_marker_right, "[%u;1m*", d + 32);
			}
		}

		// See what switches we can find on this row.
		int switch_indexes[SWITCHES_PER_ROW];
		for (unsigned num = 0; num < SWITCHES_PER_ROW; ++num) {
			switch_indexes[num] = -1;
		}
		for (unsigned i = 0; i < switches.size(); ++i) {
			if (switches[i].row == row) {
				switch_indexes[switches[i].num] = i;
			}
		}

		// Print row header.
		logprintf("[31;22m%2u (%2u-%2u)    ", row, row * 2 - 1, row * 2 + 0);

		for (unsigned num = 0; num < SWITCHES_PER_ROW; ++num) {
			const auto distro_it = switches_to_distros.find(switch_indexes[num]);
			if (distro_it == switches_to_distros.end()) {
				print_switch(g, switch_indexes[num], -1);
			} else {
				print_switch(g, switch_indexes[num], distro_it->second);
			}

			if (num == 1) {
				logprintf("%s %s", distro_marker_left, distro_marker_right);
			} else {
				logprintf("   ");
			}
		}
		logprintf("\n");

		// See if we just crossed a cap.
		for (const VerticalGap& gap : vertical_gaps) {
			if (row == gap.after_row_num) {
				logprintf("\n");
			}
		}
	}
	logprintf("[%u;22m\n", 37);

	for (unsigned i = 0; i < switches.size(); ++i) {
		const auto distro_it = switches_to_distros.find(i);
		if (distro_it == switches_to_distros.end()) {
			total_cost += _INF;
			continue;
		}
		int distro = distro_it->second;
		int this_distance = find_distance(switches[i], distro);
		Inventory this_inv = find_inventory(switches[i], distro);
		total_cost += find_cost(switches[i], distro);
		total_slack += find_slack(this_inv, this_distance);
		total_inv += this_inv;
	}

#if OUTPUT_FILES
	FILE *patchlist = fopen("patchlist.txt", "w");
	FILE *switchlist = fopen("switches.txt", "w");
	in_addr_t subnet_address = inet_addr(FIRST_SUBNET_ADDRESS);
	for (unsigned i = 0; i < switches.size(); ++i) {
		const auto distro_it = switches_to_distros.find(i);
		if (distro_it == switches_to_distros.end()) {
			continue;
		}
		int distro = distro_it->second;
		int port_num = num_ports_used[distro]++;
		fprintf(patchlist, "e%u-%u %s %s %s %s %s\n",
			switches[i].row * 2 - 1, switches[i].num + 1,
			distro_name(distro).c_str(),
			port_name(distro, port_num).c_str(),
			port_name(distro, port_num + 48).c_str(),
			port_name(distro, port_num + 96).c_str(),
			port_name(distro, port_num + 144).c_str());

		in_addr subnet_addr4;
		subnet_addr4.s_addr = subnet_address;
		fprintf(switchlist, "%s %u e%u-%u x.x.x.x\n",
			inet_ntoa(subnet_addr4), SUBNET_SIZE, switches[i].row * 2 - 1, switches[i].num + 1);
		subnet_address = htonl(ntohl(subnet_address) + (1ULL << (32 - SUBNET_SIZE)));
	}
	fclose(patchlist);
	fclose(switchlist);
#endif

#if TRUNCATE_METRIC
	logprintf("\n");
	logprintf("10m: %3u\n", total_inv.num_10m);
	logprintf("30m: %3u\n", total_inv.num_30m);
	logprintf("50m: %3u\n", total_inv.num_50m);
	logprintf("Extensions: %u\n", total_inv.extensions);
	logprintf("Horizontal gap crossings: %u\n", total_inv.horiz_gap_crossings);
	logprintf("\n");

	if (total_inv.num_10m >= _INF) {
		logprintf("Total cost: Impossible\n");
		return INT_MAX;
	}
	int total_cable = 100 * total_inv.num_10m + 300 * total_inv.num_30m + 500 * total_inv.num_50m;
#else
	// Not correct unless EXTENSION_COST = HORIZ_GAP_COST = 0, but okay.
	int total_cable = total_cost;
#endif

	logprintf("Total cable: %.1fm (cost = %.1fm)\n", total_cable / 10.0, total_cost / 10.0);
	logprintf("Total slack: %.1fm (%.2f%%)\n", total_slack / 10.0, 100.0 * double(total_slack) / double(total_cable));

	for (int i = 0; i < NUM_DISTRO; ++i) {
		Edge *e = g.source_node.edges[i];
		logprintf("Remaining ports on distro %d: %d\n", i, e->capacity - e->flow);
	}
	return total_cost;
}

void plan_recursively(int distro_placements[NUM_DISTRO], int distro_num, int min_placement, int max_placement, atomic<int> *best_cost)
{
	if (distro_num == NUM_DISTRO) {
		Planner p;
		int cost = p.do_work(distro_placements);
try_again:
		int old_best_cost = best_cost->load();
		if (cost >= old_best_cost) {
			return;
		}
		if (!best_cost->compare_exchange_weak(old_best_cost, cost)) {
			// Someone else changed the value in the meantime.
			goto try_again;
		}
		for (unsigned i = 0; i < NUM_DISTRO; ++i) {
			printf("%d ", distro_placements[i]);
		}
		printf("= %d\n", cost);

		// Do it once more, but this time with logging enabled.
		string log;
		p.set_log_buf(&log);
		p.do_work(distro_placements);
		printf("%s\n", log.c_str());

		return;
	}

	for (int i = min_placement; i <= max_placement; ++i) {
		distro_placements[distro_num] = i;
		plan_recursively(distro_placements, distro_num + 1, i + 1, max_placement, best_cost);
		distro_placements[distro_num] = -i;
		plan_recursively(distro_placements, distro_num + 1, i + 1, max_placement, best_cost);
	}
}

int main(int argc, char **argv)
{
	int distro_placements[NUM_DISTRO];
#if 0
	for (int i = 0; i < NUM_DISTRO; ++i) {
		distro_placements[i] = atoi(argv[i + 1]);
	}

	string log;
	Planner p;
	log.clear();
	p.set_log_buf(&log);
	(void)p.do_work(distro_placements);
	printf("%s\n", log.c_str());
	return 0;
#else
	atomic<int> best_cost(_INF * 1000);
	distro_placements[0] = -3;  // obvious

	constexpr int min_placement = 6;
	constexpr int max_placement = NUM_ROWS;

	// Boring single-threaded version
	// plan_recursively(distro_placements, 1, min_placement, max_placement, &best_cost);

	#pragma omp parallel for schedule(dynamic,1) collapse(2)
	for (int i = min_placement; i <= max_placement; ++i) {
		for (int j = min_placement; j <= max_placement; ++j) {
			if (j <= i) continue;

			int new_distro_placements[NUM_DISTRO];
			memcpy(new_distro_placements, distro_placements, sizeof(distro_placements));

			new_distro_placements[1] = i;

			new_distro_placements[2] = j;
			plan_recursively(new_distro_placements, 3, j + 1, max_placement, &best_cost);
			new_distro_placements[2] = -j;
			plan_recursively(new_distro_placements, 3, j + 1, max_placement, &best_cost);

			new_distro_placements[1] = -i;
			new_distro_placements[2] = j;
			plan_recursively(new_distro_placements, 3, j + 1, max_placement, &best_cost);
			new_distro_placements[2] = -j;
			plan_recursively(new_distro_placements, 3, j + 1, max_placement, &best_cost);
		}
	}

	return 0;
#endif
}
