/*******************************************************************************
 * Copyright (C) 2022-2023 Olivier Delaneau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/

#include <objects/compute_job.h>

using namespace std;

#define MAX_OVERLAP_HETS 0.75f
#define N_RANDOM_HAPS 100

compute_job::compute_job(variant_map & _V, genotype_set & _G, conditioning_set & _H, unsigned int n_max_transitions, unsigned int n_max_missing) : V(_V), G(_G), H(_H) {
	T = vector < double > (n_max_transitions, 0.0);
	M = vector < float > (n_max_missing , 0.0);
	Ordering = vector < unsigned int > (H.n_hap);
	iota(Ordering.begin(), Ordering.end(), 0);
	Oiterator = 0;
}

compute_job::~compute_job() {
	free();
}

void compute_job::free () {
	vector < double > ().swap(T);
	vector < float > ().swap(M);
	vector < vector < unsigned int > > ().swap(Kstates);
	Kbanned.clear();
	Windows.clear();
}

void compute_job::make(unsigned int ind, double min_window_size) {
	//1. Mapping coordinates of each segment
	int n_windows = Windows.build (V, G.vecG[ind], min_window_size);

	//2. Update conditional haps
	unsigned long addr_offset = H.sites_pbwt_ngroups * H.n_ind * 2UL;
	Kstates = vector < vector < unsigned int > > (n_windows, vector < unsigned int >());
	unsigned long curr_hap0 = 2*ind+0, curr_hap1 = 2*ind+1;
	for (int w = 0 ; w < n_windows ; w++) {
		vector < int > phap = vector < int > (2 * H.depth, -1);
		for (int l = Windows.W[w].start_locus ; l <= Windows.W[w].stop_locus ; l++) {
			if (H.sites_pbwt_selection[l]) {
				for (int s = 0 ; s < H.depth ; s ++) {
					int cond_hap0 = H.indexes_pbwt_neighbour[s * addr_offset + curr_hap0 * H.sites_pbwt_ngroups + H.sites_pbwt_grouping[l]];
					int cond_hap1 = H.indexes_pbwt_neighbour[s * addr_offset + curr_hap1 * H.sites_pbwt_ngroups + H.sites_pbwt_grouping[l]];
					if ((cond_hap0 >= 0) && (cond_hap0 != phap[2*s+0])) { Kstates[w].push_back(cond_hap0); phap[2*s+0] = cond_hap0; };
					if ((cond_hap1 >= 0) && (cond_hap1 != phap[2*s+1])) { Kstates[w].push_back(cond_hap1); phap[2*s+1] = cond_hap1; };
				}
			}
		}
		sort(Kstates[w].begin(), Kstates[w].end());
		Kstates[w].erase(unique(Kstates[w].begin(), Kstates[w].end()), Kstates[w].end());
	}

	//3. Protect for IBD2
	Kbanned.clear();
	for (int w = 0 ; w < n_windows; w++) {
		vector < int > toBeRemoved;

		//3.1. Identify potential IBD2 pairs
		for (int k = 1; k < Kstates[w].size() ; k++) {
			unsigned int ind0 = Kstates[w][k-1]/2;
			unsigned int ind1 = Kstates[w][k]/2;
			if (ind0 == ind1 && ind0 < G.n_ind && !G.vecG[ind0]->haploid) {
				float het_overlap = H.H_opt_hap.getMatchHets(ind, ind0, Windows.W[w].start_locus, Windows.W[w].stop_locus);
				if (het_overlap > 0.75) {
					toBeRemoved.push_back(k-1);
					toBeRemoved.push_back(k);
					Kbanned.emplace_back(ind0, Windows.W[w].start_locus, Windows.W[w].stop_locus);
					//cout << "IBD2 : " << G.vecG[ind]->name << " vs " << G.vecG[ind0]->name << " / P = " << stb.str(het_overlap*100.0, 2) << endl;
				}
			}
		}

		//3.2. Remove potential IBD2 states from conditioning set
		if (toBeRemoved.size() > 0) {
			vector < unsigned int > Ktmp;
			Ktmp.reserve(Kstates[w].size() - toBeRemoved.size());
			for (int k = 0, p = 0; k < Kstates[w].size() ; k++) {
				if (p < toBeRemoved.size() && toBeRemoved[p] == k) p++;
				else Ktmp.push_back(Kstates[w][k]);
			}

			sort(Ktmp.begin(), Ktmp.end());
			Ktmp.erase(unique(Ktmp.begin(), Ktmp.end()), Ktmp.end());
			Kstates[w] = Ktmp;
		}
	}

	//4. Protect for #states = 0
	for (int w = 0 ; w < n_windows; w++) {
		if (Kstates[w].size() < 2) {
			for (int i = 0 ; i < N_RANDOM_HAPS ; i++) {
				int random_state = Ordering[Oiterator];
				if (random_state/2 != ind) Kstates[w].push_back(random_state);
				Oiterator=((Oiterator+1)==H.n_hap)?0:(Oiterator+1);
			}
			sort(Kstates[w].begin(), Kstates[w].end());
			Kstates[w].erase(unique(Kstates[w].begin(), Kstates[w].end()), Kstates[w].end());
			vrb.warning("No PBWT states found [" + G.vecG[ind]->name  + " / w=" + stb.str(w) + "] / Using " + stb.str(Kstates[w].size()) + " random states");
		}
	}
}
