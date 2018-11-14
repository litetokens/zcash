#include <boost/array.hpp>
#include <boost/variant.hpp>
#include <iostream>
#include <vector>


#include "amount.h"
#include "asyncrpcoperation.h"
#include "base58.h"
#include "consensus/consensus.h"
#include "paymentdisclosure.h"
#include "primitives/transaction.h"
#include "random.h"
#include "script/script.h"
#include "serialize.h"
#include "streams.h"
#include "uint256.h"
#include "wallet/wallet.h"
#include "zcash/Address.hpp"
#include "zcash/JoinSplit.hpp"
#include "zcash/NoteEncryption.hpp"
#include "zcash/Proof.hpp"
#include "zcash/Zcash.h"
#include <libsnark/common/profiling.hpp>

#include "GenerateProofServer.h"
#include "geneproof.grpc.pb.h"
#include "geneproof.pb.h"

using ::protocol::ZeroProofConstant;

const int CONST_32 = 32;

//#define LogWarn (printf("%s(%d)-<%s>: ", __FILE__, __LINE__, __FUNCTION__), printf)

bool GenerateProofServer::Init()
{
    if (NULL != params) {
        delete params;
    }

    std::string vkFile = (ZC_GetParamsDir() / "sprout-verifying.key").string();
    std::string pkFile = (ZC_GetParamsDir() / "sprout-proving.key").string();

    params = ZCJoinSplit::Prepared(vkFile, pkFile);
    if (NULL == params) {
        LogError("Open verify file(%s) or prove file(%s) failue", vkFile.c_str(), pkFile.c_str());
        return false;
    }
    return true;
}

::grpc::Status GenerateProofServer::proof(::grpc::ServerContext* context,
                                          const ::protocol::ProofInputMsg* request,
                                          ::protocol::ProofOutputMsg* response)
{
    try {
        DoWork(context, request, response);
    } catch (const exception& e) {
        LogWarn("exception:%s \n", string(e.what()).c_str());
        if (response != NULL && response->has_ret()) {
            // 如果已经设置，则忽略
            if (response->ret().result_code() == 0) {
                ::protocol::Result* result = response->mutable_ret();
                result->set_result_code(2);
                result->set_result_desc("unknown error");
            }
        }
    } catch (...) {
        LogWarn("unknown error\n");
        if (response != NULL) {
            ::protocol::Result* result = response->mutable_ret();
            result->set_result_code(2);
            result->set_result_desc("unknown error");
        }
    }
    return Status::OK;
}

void GenerateProofServer::DoWork(::grpc::ServerContext* context,
                                 const ::protocol::ProofInputMsg* request,
                                 ::protocol::ProofOutputMsg* response)
{
    // 定义返回参数
    ::protocol::Result ret;
    ret.set_result_code(0);
    ret.set_result_desc("success");

    boost::array<uint256, ZC_NUM_JS_INPUTS> nullifiers;
    boost::array<uint256, ZC_NUM_JS_OUTPUTS> commitments;
    boost::array<libzcash::SproutNote, ZC_NUM_JS_OUTPUTS> notes;
    boost::array<ZCNoteEncryption::Ciphertext, ZC_NUM_JS_OUTPUTS> ciphertexts = {{{{0}}}};
    boost::array<uint256, ZC_NUM_JS_INPUTS> macs;
    // Ephemeral key
    uint256 ephemeralKey;
    uint256 randomSeed;
    uint256 esk; // payment disclosure

    boost::variant<libzcash::ZCProof, libzcash::GrothProof> proof;

    do {
        // 参数的基本判断
        if (request == NULL) {
            ret.set_result_code(1);
            ret.set_result_desc("Invalid param. request = NULL.");
            throw std::invalid_argument("Invalid param. request = NULL");
        }

        // 获取传递进来的参数
        std::vector<libzcash::JSInput> vjsin;
        GetJSInput(request, vjsin, ret);

        std::vector<libzcash::JSOutput> vjsout;
        GetJSOutput(request, vjsout, ret);

        if (!request->has_pubkeyhash() || !request->has_rt()) {
            ret.set_result_code(1);
            ret.set_result_desc("Invalid param. All param(pubkey or rt ) should be filled.");
            throw std::invalid_argument("Invalid param. request.pubkey or request.rt isn't set");
        }

        // Generate the proof, this can take over a minute.
        boost::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> inputs{vjsin[0], vjsin[1]};
        boost::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> outputs{vjsout[0], vjsout[1]};

        bool computeProof = request->compute_proof();

        if (request->pubkeyhash().hash().size() != CONST_32 ||
            request->rt().hash().size() != CONST_32) {
            ret.set_result_code(1);
            ret.set_result_desc("Invalid param. Uint256Msg(pubkey or rt) hash size should be 32");
            throw std::invalid_argument("Invalid param. Uint256Msg(pubkey or rt) hash id not equal 32");
        }
        std::vector<unsigned char> vec(CONST_32);
        GetVecStr(request->pubkeyhash().hash(), vec);
        uint256 pubKeyHash(vec);
        GetVecStr(request->rt().hash(), vec);
        uint256 anchor(vec);

        CAmount vpub_old(request->vpub_old());
        CAmount vpub_new(request->vpub_new());

        LogDebug("Start to generate proof -->\n");
        boost::array<size_t, ZC_NUM_JS_INPUTS> inputMap = {0, 1};
        boost::array<size_t, ZC_NUM_JS_OUTPUTS> outputMap = {0, 1};
        MappedShuffle(inputs.begin(), inputMap.begin(), ZC_NUM_JS_INPUTS, GetRandInt);
        MappedShuffle(outputs.begin(), outputMap.begin(), ZC_NUM_JS_OUTPUTS, GetRandInt);
        clock_t t1 = clock();

        /////  调用底层函数生成proof  ////////
        proof = params->prove(
            inputs,
            outputs,
            notes,
            ciphertexts,
            ephemeralKey,
            pubKeyHash,
            randomSeed,
            macs,
            nullifiers,
            commitments,
            vpub_old,
            vpub_new,
            anchor,
            computeProof,
            &esk // payment disclosure
        );
        clock_t t2 = clock();
        LogDebug("<--- End to generate proof\n");

        printf("proof time:%ld \n", t2 - t1);
        printf("\n\n printf nullifiers and commitments:\n");
        for (int i = 0; i < nullifiers.size(); i++) {
            printf("%s ", nullifiers[i].GetHex().c_str());
        }
        printf("\n");
        for (int i = 0; i < commitments.size(); i++) {
            printf("%s ", commitments[i].GetHex().c_str());
        }
        printf("\n\n");

        // 自校验
        libzcash::ZCProof zcProof = boost::get<libzcash::ZCProof>(proof);
        auto verifier = libzcash::ProofVerifier::Strict();
        if (!params->verify(zcProof,
                            verifier,
                            pubKeyHash,
                            randomSeed,
                            macs,
                            nullifiers,
                            commitments,
                            vpub_old,
                            vpub_new,
                            anchor)) {
            ret.set_result_code(3);
            ret.set_result_desc("error verifying joinsplit");
            throw std::runtime_error("----> verify failure!!! <----");
        } else {
            LogDebug("----> verify success ^_^ <----\n");
        }

    } while (0);

    LogDebug("Start to format response message\n");
    if (response != NULL) {
        if (ret.result_code() == 0) {
            //boost::array<libzcash::SproutNote, ZC_NUM_JS_OUTPUTS> notes;
            for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
                ::protocol::SproutNoteMsg* sprouteNote = response->add_out_notes();
                sprouteNote->set_value(notes[i].value());
                ::protocol::Uint256Msg* uint256Msg = sprouteNote->mutable_a_pk();
                uint256Msg->set_hash(notes[i].a_pk.begin(), notes[i].a_pk.size());
                uint256Msg = sprouteNote->mutable_rho();
                uint256Msg->set_hash(notes[i].rho.begin(), notes[i].rho.size());
                uint256Msg = sprouteNote->mutable_r();
                uint256Msg->set_hash(notes[i].r.begin(), notes[i].r.size());
            }

            //boost::array<ZCNoteEncryption::Ciphertext, ZC_NUM_JS_OUTPUTS> ciphertexts
            for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
                response->add_out_ciphertexts(ciphertexts[i].begin(), ciphertexts[i].size());
            }

            // uint256 ephemeralKey;
            ::protocol::Uint256Msg* uintMsg = response->mutable_out_ephemeralkey();
            uintMsg->set_hash(ephemeralKey.begin(), ephemeralKey.size());
            // uint256 randomSeed;
            uintMsg = response->mutable_out_randomseed();
            uintMsg->set_hash(randomSeed.begin(), randomSeed.size());
            // boost::array<uint256, ZC_NUM_JS_INPUTS> macs;
            for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
                uintMsg = response->add_out_macs();
                uintMsg->set_hash(macs[i].begin(), macs[i].size());
            }

            // boost::array<uint256, ZC_NUM_JS_INPUTS> nullifiers;
            for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
                uintMsg = response->add_out_nullifiers();
                uintMsg->set_hash(nullifiers[i].begin(), nullifiers[i].size());
            }

            // boost::array<uint256, ZC_NUM_JS_OUTPUTS> commitments;
            for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
                uintMsg = response->add_out_commitments();
                uintMsg->set_hash(commitments[i].begin(), commitments[i].size());
            }

            // uint256 esk;
            uintMsg = response->mutable_out_esk();
            uintMsg->set_hash(esk.begin(), esk.size());

            libzcash::ZCProof zcProof = boost::get<libzcash::ZCProof>(proof);
            std::vector<unsigned char> vecProof;
            zcProof.GetProofData(vecProof);

            printf("---> vecproof: %lu \n", vecProof.size());
            response->set_proof(&(*vecProof.begin()), vecProof.size());
        }

        ::protocol::Result* result = response->mutable_ret();
        result->set_result_code(ret.result_code());
        result->set_result_desc(ret.result_desc());
    }

    LogDebug("Generate proof success!\n");
}

// 未考虑参数的校验
void GenerateProofServer::GetJSInput(
    const ::protocol::ProofInputMsg* request,
    std::vector<libzcash::JSInput>& input,
    ::protocol::Result& resultCode)
{
    // 内部参数，到这里的话request不可能为空，暂不判断
    std::vector<unsigned char> vec(CONST_32);

    for (size_t i = 0; i < request->inputs_size(); i++) {
        const ::protocol::JSInputMsg& inputMsg = request->inputs(i);

        if (!inputMsg.has_witness() || !inputMsg.has_note() || !inputMsg.has_key()) {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. All param(witness,note,key) should be filled.");
            throw std::invalid_argument("Invalid param. inputs(witness note inputs) is not set");
        }

        // ZCIncrementalWitness witness
        const ::protocol::IncrementalWitnessMsg& incrementalWitness = inputMsg.witness();

        ZCIncrementalMerkleTree tree;
        if (incrementalWitness.has_tree()) {
            LogDebug("Deal ZCIncrementalMerkleTree tree\n");
            tree = GetIncrementalMerkleTree(&incrementalWitness.tree(), resultCode);
        }

        ZCIncrementalMerkleTree cursor;
        if (incrementalWitness.has_cursor()) {
            LogDebug("Deal ZCIncrementalMerkleTree cursor\n");
            cursor = GetIncrementalMerkleTree(&incrementalWitness.cursor(), resultCode);
        }

        size_t cursor_depth = incrementalWitness.cursor_depth();

        vector<libzcash::SHA256Compress> filled;
        for (int i = 0; i < incrementalWitness.filled_size(); i++) {
            if (incrementalWitness.filled(i).hash().size() == CONST_32) {
                GetVecStr(incrementalWitness.filled(i).hash(), vec);
                filled.push_back(uint256(vec));
            } else {
                resultCode.set_result_code(1);
                resultCode.set_result_desc("Invalid param. Uint256Msg(incrementalWitness array) hash size should be 32");
                throw std::invalid_argument("Invalid param. Uint256Msg(incrementalWitness array) hash size id not equal 32");
            }
        }
        ZCIncrementalWitness witness(tree, filled, cursor, cursor_depth);

        // SproutNote note;
        const ::protocol::SproutNoteMsg& sproutNote = inputMsg.note();

        uint64_t value = sproutNote.value();
        if (sproutNote.a_pk().hash().size() != CONST_32 ||
            sproutNote.rho().hash().size() != CONST_32 ||
            sproutNote.r().hash().size() != CONST_32) {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. Uint256Msg(a_pk,rho,r) hash size should be 32");
            throw std::invalid_argument("Invalid param. Uint256Msg(a_pk,rho,r) hash size is not equal 32");
        }

        GetVecStr(sproutNote.a_pk().hash(), vec);
        uint256 a_pk(vec);
        GetVecStr(sproutNote.rho().hash(), vec);
        uint256 rho(vec);
        GetVecStr(sproutNote.r().hash(), vec);
        uint256 r(vec);

        libzcash::SproutNote note(a_pk, value, rho, r);

        // SpendingKey key;
        const ::protocol::Uint256Msg& keyMsg = inputMsg.key();
        if (keyMsg.hash().size() != CONST_32) {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. Uint256Msg(key) hash size should be 32");
            throw std::invalid_argument("Invalid param. Uint256Msg(key) hash size is not equal 32");
        }

        GetVecStr(keyMsg.hash(), vec);
        uint256 key256(vec);
        uint252 key252(key256);
        libzcash::SpendingKey key(key252);

        input.push_back(libzcash::JSInput(witness, note, key));
    }

    printf("--->>input size: %lu \n", input.size());

    // 如果不足，则添加
    while (input.size() < ZC_NUM_JS_INPUTS) {
        input.push_back(libzcash::JSInput());
    }
}

void GenerateProofServer::GetJSOutput(
    const ::protocol::ProofInputMsg* request,
    std::vector<libzcash::JSOutput>& output,
    ::protocol::Result& resultCode)
{
    for (size_t i = 0; i < request->outputs_size(); i++) {
        const ::protocol::JSOutputMsg& outputMsg = request->outputs(i);

        if (!outputMsg.has_a_pk() || !outputMsg.has_pk_enc()) {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. JSOutputMsg all param should be filled");
            throw std::invalid_argument("Invalid param. output(a_pk pk_enc) is not set");
        }

        if (outputMsg.a_pk().hash().size() != CONST_32 ||
            outputMsg.pk_enc().hash().size() != CONST_32) {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. Uint256Msg(a_pk or pk_enc) hash size should be 32");
            throw std::invalid_argument("Invalid param. Uint256Msg(a_pk or pk_enc) hash size is not equal 32");
        }

        //  PaymentAddress addr;
        std::vector<unsigned char> vec(CONST_32);
        GetVecStr(outputMsg.a_pk().hash(), vec);
        uint256 a_pk(vec);

        std::string str = outputMsg.pk_enc().hash();
        GetVecStr(outputMsg.pk_enc().hash(), vec);
        uint256 pk_enc(vec);

        std::string s = HexStr(str.begin(), str.end());

        printf("pk_enc(in):%s pk_enc(out):%s\n", s.c_str(), pk_enc.GetHex().c_str());

        libzcash::PaymentAddress addr(a_pk, pk_enc);

        // uint64_t value;
        uint64_t value = outputMsg.value();

        // boost::array<unsigned char, ZC_MEMO_SIZE> memo = {{0xF6}};
        boost::array<unsigned char, ZC_MEMO_SIZE> memo = {{0xF6}};
        // copy vector into boost array
        const string& strMomo = outputMsg.memo();
        int lenMemo = strMomo.size();
        for (int i = 0; i < ZC_MEMO_SIZE && i < lenMemo; i++) {
            memo[i] = strMomo[i];
        }

        libzcash::JSOutput jsoutput(addr, value);
        jsoutput.memo = memo;

        output.push_back(jsoutput);
    }
    LogDebug("--->> Get output size: %lu \n", output.size());

    // 如果不足，则添加
    while (output.size() < ZC_NUM_JS_INPUTS) {
        output.push_back(libzcash::JSOutput());
    }
}

ZCIncrementalMerkleTree GenerateProofServer::GetIncrementalMerkleTree(
    const ::protocol::IncrementalMerkleTreeMsg* merkleTreeMsg,
    ::protocol::Result& resultCode)
{
    ZCIncrementalMerkleTree emptytree;

    if (merkleTreeMsg == NULL) {
        resultCode.set_result_code(1);
        resultCode.set_result_desc("Invalid param. IncrementalMerkleTreeMsg = NULL");
        throw std::invalid_argument("Invalid param. merkleTreeMsg = NULL");
    }

    // parent 的大小不超过29
    if (merkleTreeMsg->parents_size() > ::protocol::TRON_INCREMENTAL_MERKLE_TREE_DEPTH) {
        resultCode.set_result_code(1);
        resultCode.set_result_desc("Invalid param. parents size should no more than 29");
        throw std::invalid_argument("Invalid param. parents size should no more than 29");
    }

    std::vector<unsigned char> vec(CONST_32);
    std::vector<boost::optional<libzcash::SHA256Compress>> parents;
    if (merkleTreeMsg->parents_size() > 0) {
        for (int i = 0; i < merkleTreeMsg->parents_size(); i++) {
            if (merkleTreeMsg->parents(i).hash().size() == CONST_32) {
                GetVecStr(merkleTreeMsg->parents(i).hash(), vec);
                parents.push_back(libzcash::SHA256Compress(uint256(vec)));
            } else {
                resultCode.set_result_code(1);
                resultCode.set_result_desc("Invalid param. Uint256Msg(parents(i)) hash size should be 32");
                throw std::invalid_argument("Invalid param. Uint256Msg (parents(i)) hash size id not equal 32");
            }
        }
    }

    boost::optional<libzcash::SHA256Compress> left = boost::none;
    boost::optional<libzcash::SHA256Compress> right = boost::none;
    if (merkleTreeMsg->has_left() && merkleTreeMsg->left().hash().size() != 0) {
        if (merkleTreeMsg->left().hash().size() == CONST_32) {
            GetVecStr(merkleTreeMsg->left().hash(), vec);
            left = libzcash::SHA256Compress(uint256(vec));
        } else {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. Uint256Msg(left) hash size should be 32");
            throw std::invalid_argument("Invalid param. Uint256Msg(left) hash size not equal 32");
        }
    }
    if (merkleTreeMsg->has_right() && merkleTreeMsg->right().hash().size() != 0) {
        if (merkleTreeMsg->right().hash().size() == CONST_32) {
            GetVecStr(merkleTreeMsg->right().hash(), vec);
            left = libzcash::SHA256Compress(uint256(vec));
        } else {
            resultCode.set_result_code(1);
            resultCode.set_result_desc("Invalid param. Uint256Msg(right) hash size should be 32");
            throw std::invalid_argument("Invalid param. Uint256Msg(right) hash size id not equal 32");
        }
    }

    ZCIncrementalMerkleTree merkleTree(left, right, parents);

    LogDebug("Finish deal GetIncrementalMerkleTree OK\n");

    return merkleTree;
}
