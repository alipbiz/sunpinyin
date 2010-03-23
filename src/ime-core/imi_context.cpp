/*
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS HEADER.
 * 
 * Copyright (c) 2007 Sun Microsystems, Inc. All Rights Reserved.
 * 
 * The contents of this file are subject to the terms of either the GNU Lesser
 * General Public License Version 2.1 only ("LGPL") or the Common Development and
 * Distribution License ("CDDL")(collectively, the "License"). You may not use this
 * file except in compliance with the License. You can obtain a copy of the CDDL at
 * http://www.opensource.org/licenses/cddl1.php and a copy of the LGPLv2.1 at
 * http://www.opensource.org/licenses/lgpl-license.php. See the License for the 
 * specific language governing permissions and limitations under the License. When
 * distributing the software, include this License Header Notice in each file and
 * include the full text of the License in the License file as well as the
 * following notice:
 * 
 * NOTICE PURSUANT TO SECTION 9 OF THE COMMON DEVELOPMENT AND DISTRIBUTION LICENSE
 * (CDDL)
 * For Covered Software in this distribution, this License shall be governed by the
 * laws of the State of California (excluding conflict-of-law provisions).
 * Any litigation relating to this License shall be subject to the jurisdiction of
 * the Federal Courts of the Northern District of California and the state courts
 * of the State of California, with venue lying in Santa Clara County, California.
 * 
 * Contributor(s):
 * 
 * If you wish your version of this file to be governed by only the CDDL or only
 * the LGPL Version 2.1, indicate your decision by adding "[Contributor]" elects to
 * include this software in this distribution under the [CDDL or LGPL Version 2.1]
 * license." If you don't indicate a single choice of license, a recipient has the
 * option to distribute your version of this file under either the CDDL or the LGPL
 * Version 2.1, or to extend the choice of license to its licensees as provided
 * above. However, if you add LGPL Version 2.1 code and therefore, elected the LGPL
 * Version 2 license, then the option applies only if the new code is made subject
 * to such option by the copyright holder. 
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <algorithm>
#include "imi_defines.h"
#include "imi_context.h"

TCandiRank::TCandiRank(bool user, bool best, unsigned len,
                       bool fromLattice, TSentenceScore score)
{
    anony.m_user = (user)?0:1;
    anony.m_best = (best)?0:1;
    anony.m_len = (len > 31)?(0):(31-len);
    anony.m_lattice = (fromLattice)?0:1;

    double ds = -score.log2();

    //make it 24-bit
    if (ds > 32767.0)
        ds = 32767.0;
    else if (ds < -32768.0)
        ds = -32768.0;
    unsigned cost = unsigned((ds+32768.0)*256.0);
    anony.m_cost = cost;
}

TCandiRank::TCandiRank(bool user, bool best, unsigned len,
                       bool fromLattice, unsigned rank)
{
    anony.m_user = (user)?0:1;
    anony.m_best = (best)?0:1;
    anony.m_len = (len > 31)?(0):(31-len);
    anony.m_lattice = (fromLattice)?0:1;
    anony.m_cost = rank;
}

void CLatticeFrame::print (std::string prefix)
{
    if (m_bwType & BESTWORD) printf ("B");
    if (m_bwType & USER_SELECTED) printf ("U");
    printf ("\n");

    prefix += "    ";
    printf ("  Lexicon States:\n");
    for_each (m_lexiconStates.begin (), m_lexiconStates.end (), 
              bind2nd (mem_fun_ref (&TLexiconState::print), prefix));

    printf ("  Lattice States:\n");
    for_each (m_latticeStates.begin (), m_latticeStates.end (), 
              bind2nd (mem_fun_ref (&TLatticeState::print), prefix));
    printf ("\n");
}

void CIMIContext::printLattice ()
{
    std::string prefix;

    for (int i=0; i<=m_tailIdx; ++i) {
        if (m_lattice[i].m_type == CLatticeFrame::UNUSED)
            continue;

        printf ("Lattice Frame [%d]:", i);
        m_lattice[i].print (prefix);
    }
}

// FIXME: a dirty way to save a reference of latest segments
IPySegmentor::TSegmentVec g_dummy_segs;
CIMIContext::CIMIContext () 
    : m_tailIdx(1), m_pModel(NULL), m_pPinyinTrie(NULL), m_pUserDict(NULL), m_pHistory(NULL), 
      m_historyPower(3), m_bFullSymbolForwarding(false), m_pGetFullSymbolOp(NULL),
      m_bFullPunctForwarding(true), m_pGetFullPunctOp(NULL), m_bDynaCandiOrder(true),
      m_candiStarts(0), m_candiEnds(0), m_csLevel(0), m_bNonCompleteSyllable(true), m_latestSegments(g_dummy_segs)
{
    m_lattice.resize (MAX_LATTICE_LENGTH);
    m_lattice[0].m_latticeStates.push_back (TLatticeState (-1.0, 0));
}

void CIMIContext::setCoreData (CIMIData *pCoreData)
{
    m_pModel = pCoreData->getSlm();
    m_pPinyinTrie = pCoreData->getPinyinTrie();
}

void CIMIContext::clear ()
{
    _clearFrom (1);
    m_tailIdx = 1;
    m_candiStarts = m_candiEnds = 0;
}

void CIMIContext::_clearFrom (unsigned idx)
{
    for (int i=idx; i<m_tailIdx+1; ++i)
        m_lattice[i].clear();
}

bool CIMIContext::buildLattice (IPySegmentor::TSegmentVec &segments, unsigned rebuildFrom, bool doSearch)
{
    m_latestSegments = segments;

    _clearFrom (rebuildFrom);

    IPySegmentor::TSegmentVec::iterator it  = segments.begin ();
    IPySegmentor::TSegmentVec::iterator ite = segments.end ();
   
    unsigned i, j=0; 
    for (; it != ite; ++it) {
        i = it->m_start;
        j = i + it->m_len;

        if (i < rebuildFrom-1)
            continue;

        if (j >= m_lattice.capacity()-1)
            break;

        if (it->m_type == IPySegmentor::SYLLABLE)
            _forwardSyllables (i, j, it->m_syllables);
        else if (it->m_type == IPySegmentor::SYLLABLE_SEP)
            _forwardSyllableSep (i, j);
        else
            _forwardString (i, j, it->m_syllables);
    }

    _forwardTail (j, j+1);
    m_tailIdx = j+1;

    return doSearch && searchFrom (rebuildFrom);
}

void CIMIContext::_forwardSyllables (unsigned i, unsigned j, std::vector<unsigned>& syllables)
{
    std::vector<unsigned>::iterator it  = syllables.begin ();
    std::vector<unsigned>::iterator ite = syllables.end ();

    _forwardSingleSyllable (i, j, *it, false);
    for (++it; it != ite; ++it)
        _forwardSingleSyllable (i, j, *it, true);
}


void CIMIContext::_forwardString (unsigned i, unsigned j, std::vector<unsigned>& strbuf)
{
    if (strbuf.size() == 1) {
        unsigned ch = strbuf[0];
        ispunct(ch)? _forwardPunctChar (i, j, ch): _forwardOrdinaryChar (i, j, ch);
    } else{
        CLatticeFrame &fr = m_lattice[j];
        fr.m_wstr.assign (strbuf.begin(), strbuf.end());
        fr.m_lexiconStates.push_back (TLexiconState(i, 0));
    }
}

void CIMIContext::_forwardSingleSyllable (unsigned i, unsigned j, TSyllable syllable, bool isFuzzy)
{
    const CPinyinTrie::TNode * pn = NULL;

    CLatticeFrame &fr = m_lattice[j];
    fr.m_type = CLatticeFrame::SYLLABLE;

    CLexiconStates::iterator it  = m_lattice[i].m_lexiconStates.begin ();
    CLexiconStates::iterator ite = m_lattice[i].m_lexiconStates.end ();
    for (; it != ite; ++it) {
        TLexiconState &lxst = *it;
        bool added_from_sysdict = false;

        if (lxst.m_pPYNode) {
            // try to match a word from lattice i to lattice j
            // and if match, we'll count it as a new lexicon on lattice j
            pn = m_pPinyinTrie->transfer (lxst.m_pPYNode, syllable);
            if (pn) {
                added_from_sysdict = true;
                TLexiconState new_lxst = TLexiconState (lxst.m_start, pn, lxst.m_syls, isFuzzy);
                new_lxst.m_syls.push_back (syllable);
                fr.m_lexiconStates.push_back (new_lxst);
            }
        }

        if (m_pUserDict && lxst.m_syls.size() < MAX_USRDEF_WORD_LEN) {
            // try to match a word from user dict
            CSyllables syls = lxst.m_syls;
            syls.push_back (syllable);
            std::vector<CPinyinTrie::TWordIdInfo> words;
            m_pUserDict->getWords (syls, words);
            if (!words.empty() || !added_from_sysdict) {
                // even if the words is empty we'll add a fake lexicon
                // here. This helps _saveUserDict detect new words.
                TLexiconState new_lxst = TLexiconState (lxst.m_start, lxst.m_syls, words, isFuzzy);
                new_lxst.m_syls.push_back (syllable);
                fr.m_lexiconStates.push_back (new_lxst);
            }
        }
    }

    // last, create a lexicon for single character with only one syllable
    pn = m_pPinyinTrie->transfer (syllable);
    if (pn) {
        CSyllables syls;
        syls.push_back (syllable);
        TLexiconState new_lxst = TLexiconState (i, pn, syls, isFuzzy);
        fr.m_lexiconStates.push_back (new_lxst);
    }
}

void CIMIContext::_forwardSyllableSep (unsigned i, unsigned j)
{
    CLatticeFrame &fr = m_lattice[j];
    fr.m_type = CLatticeFrame::SYLLABLE | CLatticeFrame::SYLLABLE_SEP;
    fr.m_lexiconStates = m_lattice[i].m_lexiconStates;
}

void CIMIContext::_forwardPunctChar (unsigned i, unsigned j, unsigned ch)
{
    CLatticeFrame &fr = m_lattice[j];

    wstring wstr; 
    unsigned wid = 0;

    if (m_pGetFullPunctOp) {
        wstr = (*m_pGetFullPunctOp) (ch);
        wid = m_pPinyinTrie->getSymbolId (wstr);

        if (!m_bFullPunctForwarding)
            wstr.clear ();
    }

    fr.m_type = CLatticeFrame::PUNC;

    if (!wstr.empty())
        fr.m_wstr = wstr;
    else
        fr.m_wstr.push_back (ch);

    fr.m_lexiconStates.push_back (TLexiconState(i, wid));
}

void CIMIContext::_forwardOrdinaryChar (unsigned i, unsigned j, unsigned ch)
{
    CLatticeFrame &fr = m_lattice[j];

    wstring wstr; 
    unsigned wid = 0;

    if (m_pGetFullSymbolOp) {
        wstr = (*m_pGetFullSymbolOp) (ch);
        wid = m_pPinyinTrie->getSymbolId (wstr);

        if (!m_bFullSymbolForwarding)
            wstr.clear ();
    }

    fr.m_type = wid? CLatticeFrame::SYMBOL: CLatticeFrame::ASCII;

    if (!wstr.empty ())
        fr.m_wstr = wstr;
    else
        fr.m_wstr.push_back (ch);

    fr.m_lexiconStates.push_back (TLexiconState(i, wid));
}

void CIMIContext::_forwardTail (unsigned i, unsigned j)
{
    CLatticeFrame &fr = m_lattice[j];
    fr.m_type = CLatticeFrame::TAIL;
    fr.m_lexiconStates.push_back (TLexiconState (i, OOV_WORD_ID));
}

bool CIMIContext::searchFrom (unsigned idx)
{
    bool affectCandidates = (idx <= m_candiEnds);

    _clearBestPath ();

    for (; idx<=m_tailIdx; ++idx) {
        CLatticeFrame &fr = m_lattice[idx];

        if (fr.m_type == CLatticeFrame::UNUSED)
            continue;

        fr.m_latticeStates.clear ();

        /* user selected word might be cut in next step */
        if (fr.m_bwType & CLatticeFrame::USER_SELECTED)
            _transferBetween (fr.m_bestWord.m_start, idx, fr.m_bestWord.m_wordId);

        CLexiconStates::iterator it  = fr.m_lexiconStates.begin ();
        CLexiconStates::iterator ite = fr.m_lexiconStates.end ();
        for (; it != ite; ++it) {
            unsigned word_num = 0;
            TLexiconState &lxst = *it;
            const CPinyinTrie::TWordIdInfo *words = lxst.getWords (word_num);

            if (!word_num)
                continue;

            if (lxst.m_start == m_candiStarts && idx > m_candiEnds)
                affectCandidates = true;

            /* only selected the word with higher unigram probablities */
            int maxsz = it->m_bFuzzy? MAX_LEXICON_TRIES>1: MAX_LEXICON_TRIES;
            int sz = word_num<maxsz? word_num: maxsz;
            int i = 0, count = 0;
            double ic = it->m_bFuzzy? 0.5: 1.0;
            for (i = 0; count < sz && i < sz && (words[i].m_bSeen || count < 2); ++i) {
                if (m_csLevel >= words[i].m_csLevel) {
                    _transferBetween (lxst.m_start, idx, words[i].m_id, ic);
                    ++ count;
                }
            }

            /* try extra words in history cache */
            if (m_pHistory) {
                for (; i < word_num; ++i) {
                    if (m_csLevel >= words[i].m_csLevel && m_pHistory->seenBefore (words[i].m_id))
                        _transferBetween (lxst.m_start, idx, words[i].m_id, ic);
                }
            }
        }
    }

    _backTraceBestPath ();

    return affectCandidates;
}

void CIMIContext::_transferBetween (unsigned start, unsigned end, unsigned wid, double ic)
{
    CLatticeFrame &start_fr = m_lattice[start];
    CLatticeFrame &end_fr   = m_lattice[end];

    TLatticeState node (-1.0, end);
    TSentenceScore efic (ic);

    if ((end_fr.m_bwType & CLatticeFrame::USER_SELECTED) && end_fr.m_bestWord.m_wordId == wid)
        efic = TSentenceScore (30000, 1.0);

    static double s_history_distribution[11] = {0.0, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50};
    double weight_h = s_history_distribution[m_historyPower];
    double weight_s = 1.0 - weight_h;

    CLatticeStates::iterator it  = start_fr.m_latticeStates.begin();
    CLatticeStates::iterator ite = start_fr.m_latticeStates.end();

    for (; it != ite; ++it) {
        node.m_pBackTraceNode = &(*it);
        node.m_backTraceWordId = wid;

        double ts = m_pModel->transfer(it->m_slmState, wid, node.m_slmState);
        m_pModel->historify(node.m_slmState);

        // backward to psuedo root, so wid is probably a user word, save the wid in idx field,
        // so that later we could get it via CThreadSlm::lastWordId, to calculate p_{cache} correctly.
        if (node.m_slmState.getLevel() == 0 && m_pHistory && m_pHistory->seenBefore(wid))
            node.m_slmState.setIdx(wid);  // an psuedo unigram node state

        if (m_pHistory) {
            unsigned history[2] = {m_pModel->lastWordId(it->m_slmState), wid};
            double hpr = m_pHistory->pr(history, history+2);
            ts = weight_s * ts + weight_h*hpr;
        }

        node.m_score = it->m_score * efic * TSentenceScore(ts);
        end_fr.m_latticeStates.push_back (node);
    }
}

void CIMIContext::_backTraceBestPath ()
{
    CLatticeStates& tail_states = m_lattice[m_tailIdx].m_latticeStates;

    // there must be some transfer errors
    if (tail_states.size() != 1)
        return;

    TLatticeState *bs = &(tail_states[0]);

    while (bs->m_pBackTraceNode) {
        unsigned start = bs->m_pBackTraceNode->m_frIdx;
        unsigned end   = bs->m_frIdx;
        CLatticeFrame & end_fr = m_lattice[end];

        if (! (end_fr.m_bwType & CLatticeFrame::USER_SELECTED)) {
            end_fr.m_bwType |= CLatticeFrame::BESTWORD;

            end_fr.m_bestWord.m_start = start;
            end_fr.m_bestWord.m_end = end;
            end_fr.m_bestWord.m_wordId = bs->m_backTraceWordId;
            end_fr.m_bestWord.m_cwstr = end_fr.m_wstr.empty()?
                                        _getWstr (bs->m_backTraceWordId):
                                        end_fr.m_wstr.c_str();
        }

        m_bestPath.push_back (end);
        bs = bs->m_pBackTraceNode;
    }

    std::reverse (m_bestPath.begin(), m_bestPath.end());
}

void CIMIContext::_clearBestPath ()
{
    m_bestPath.clear ();
}

unsigned CIMIContext::getBestSentence (wstring& result, unsigned start, unsigned end)
{
    result.clear();

    if (UINT_MAX == end) end = m_tailIdx - 1;

    while (end > start && m_lattice[end].m_bwType == CLatticeFrame::NO_BESTWORD)
        end --;

    unsigned i = end, nWordConverted = 0;
    while (i > start) {
        CLatticeFrame &fr = m_lattice[i];
        result.insert (0, fr.m_bestWord.m_cwstr);
        i = fr.m_bestWord.m_start;
        nWordConverted ++;
    }

    return nWordConverted;
}

struct TCandiPair {
    CCandidate                      m_candi;
    TCandiRank                      m_Rank;

    TCandiPair() : m_candi(), m_Rank() { }
};

struct TCandiPairPtr {
    TCandiPair*                     m_Ptr;

    TCandiPairPtr(TCandiPair* p=NULL) : m_Ptr(p)
    { }

    bool
    operator< (const TCandiPairPtr& b) const
    { return m_Ptr->m_Rank < b.m_Ptr->m_Rank; }
};

const TWCHAR *CIMIContext::_getWstr (unsigned wid)
{
    if (wid < m_pPinyinTrie->getWordCount())
        return (*m_pPinyinTrie)[wid];
    else if (m_pUserDict)
        return (*m_pUserDict)[wid];
    else
        return NULL;
}

void CIMIContext::getCandidates (unsigned frIdx, CCandidates& result)
{
    TCandiPair cp;
    static std::map<unsigned, TCandiPair> map;
    std::map<unsigned, TCandiPair>::iterator it_map;

    map.clear();
    result.clear();

    int len = 1;
    cp.m_candi.m_start = m_candiStarts = frIdx++;

    for (;frIdx < m_tailIdx; ++frIdx, ++len)  {
        CLatticeFrame &fr = m_lattice[frIdx];

        cp.m_candi.m_end = frIdx;
        if (fr.m_bwType != CLatticeFrame::NO_BESTWORD && fr.m_bestWord.m_start == m_candiStarts) {
            cp.m_candi = fr.m_bestWord;
            cp.m_Rank = TCandiRank(fr.m_bwType & CLatticeFrame::USER_SELECTED,
                                   fr.m_bwType & CLatticeFrame::BESTWORD,
                                   0, false, 0);
            map [cp.m_candi.m_wordId] = cp;
        }

        if (!fr.isSyllableFrame ())
            continue;

        bool found = false;
        CLexiconStates::iterator it  = fr.m_lexiconStates.begin();
        CLexiconStates::iterator ite = fr.m_lexiconStates.end();
        for (; it != ite; ++it) {
            TLexiconState & lxst = *it;

            if (lxst.m_start != m_candiStarts)
                continue;

            found = true;
            unsigned word_num;
            const CPinyinTrie::TWordIdInfo *words = lxst.getWords (word_num);

            for (unsigned i=0; i<word_num; ++i) {
                if (m_csLevel < words[i].m_csLevel)
                    continue;

                cp.m_candi.m_wordId = words[i].m_id;
                cp.m_candi.m_cwstr = _getWstr (cp.m_candi.m_wordId);
                if (!cp.m_candi.m_cwstr)
                    continue;

                //sorting according to the order in PinYinTire
                cp.m_Rank = TCandiRank(false, false, len, false, i);
                it_map = map.find(cp.m_candi.m_wordId);
                if (it_map == map.end() || cp.m_Rank < it_map->second.m_Rank)
                    map[cp.m_candi.m_wordId] = cp;
            }
        }

        if (!found) break;

        if (m_bDynaCandiOrder) {
            CLatticeStates::iterator it  = fr.m_latticeStates.begin();
            CLatticeStates::iterator ite = fr.m_latticeStates.end();
            for (; it != ite; ++it) {
                TLatticeState & ltst = *it;

                if (ltst.m_pBackTraceNode->m_frIdx != m_candiStarts)
                    continue;

                cp.m_candi.m_wordId = ltst.m_backTraceWordId;
                cp.m_candi.m_cwstr = _getWstr (cp.m_candi.m_wordId);
                if (!cp.m_candi.m_cwstr)
                    continue;

                cp.m_Rank = TCandiRank(false, false, len, true, ltst.m_score/ltst.m_pBackTraceNode->m_score);
                it_map = map.find(cp.m_candi.m_wordId);
                if (it_map == map.end() || cp.m_Rank < it_map->second.m_Rank)
                    map[cp.m_candi.m_wordId] = cp;
            }
        }

        m_candiEnds = frIdx;
    }

    std::vector<TCandiPairPtr> vec;

    vec.reserve(map.size());
    std::map<unsigned, TCandiPair>::iterator it_mapE = map.end();
    for (it_map = map.begin(); it_map != it_mapE; ++it_map)
        vec.push_back(TCandiPairPtr(&(it_map->second)));
    std::make_heap(vec.begin(), vec.end());
    std::sort_heap(vec.begin(), vec.end());

    for (int i=0, sz=vec.size(); i < sz; ++i)
        result.push_back(vec[i].m_Ptr->m_candi);
}

unsigned CIMIContext::cancelSelection (unsigned frIdx, bool doSearch)
{
    unsigned ret = frIdx;

    CLatticeFrame &fr = m_lattice[frIdx];
    while (fr.m_bwType & CLatticeFrame::IGNORED) {
        --frIdx;
        fr = m_lattice[frIdx];
    }
    
    if (fr.m_bwType & CLatticeFrame::USER_SELECTED) {
        ret = fr.m_bestWord.m_start;
        fr.m_bwType = CLatticeFrame::NO_BESTWORD;
        if (doSearch) searchFrom (frIdx);
    }

    return ret;
}

void CIMIContext::makeSelection (CCandidate &candi, bool doSearch)
{
    CLatticeFrame &fr = m_lattice[candi.m_end];
    fr.m_bwType = fr.m_bwType | CLatticeFrame::USER_SELECTED;
    fr.m_bestWord = candi;
    if (doSearch) searchFrom (candi.m_end);
}

void CIMIContext::memorize ()
{
    _saveUserDict ();
    _saveHistoryCache ();
}

void CIMIContext::_saveUserDict ()
{
    if (!m_pUserDict)
        return;

    if (m_bestPath.empty())
        return;
    
    bool has_user_selected = false;
    std::vector<unsigned>::iterator it  = m_bestPath.begin();
    std::vector<unsigned>::iterator ite = m_bestPath.end() - 1;
    unsigned s = 0;
    for (; it != ite; ++it, ++s) {
        has_user_selected |= (m_lattice[*it].m_bwType & CLatticeFrame::USER_SELECTED);
        if (!m_lattice[*it].isSyllableFrame ())
            break;
    }

    if (has_user_selected && s >= 2) {
        CSyllables syls;
        -- it;
        CLexiconStates::iterator lxit  = m_lattice[*it].m_lexiconStates.begin();
        CLexiconStates::iterator lxite = m_lattice[*it].m_lexiconStates.end();
        for (; lxit != lxite; ++lxit) {
            if (lxit->m_start == 0 && !lxit->m_bFuzzy) {
                syls = lxit->m_syls;
                break;
            }
        }

        if (!syls.empty()) {
            wstring phrase;
            getBestSentence (phrase, 0, *it);
            m_pUserDict->addWord (syls, phrase);
        }
    }
}

void CIMIContext::_saveHistoryCache ()
{
    if (!m_pHistory)
        return;

    if (m_bestPath.empty())
        return;

    std::vector<unsigned> result;
    std::vector<unsigned>::const_iterator it  = m_bestPath.begin();
    std::vector<unsigned>::const_iterator ite = m_bestPath.end() - 1;
    for (; it != ite; ++it) {
        CLatticeFrame &fr = m_lattice[*it];
        if (fr.isSyllableFrame ())
            result.push_back (fr.m_bestWord.m_wordId);
        else 
            result.push_back (0);
    }

    if (!result.empty())
        m_pHistory->memorize (&(result[0]), &(result[0]) + result.size());

}

void CIMIContext::deleteCandidate (CCandidate &candi)
{
    unsigned wid = candi.m_wordId;

    if (wid > INI_USRDEF_WID) {
        m_pHistory->forget (wid);
        m_pUserDict->removeWord (wid);
        buildLattice (m_latestSegments, candi.m_start+1);
    }
}
