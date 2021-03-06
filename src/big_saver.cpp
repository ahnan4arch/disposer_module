//-----------------------------------------------------------------------------
// Copyright (c) 2015-2017 Benjamin Buch
//
// https://github.com/bebuch/disposer_module
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt)
//-----------------------------------------------------------------------------
#include "bitmap_sequence.hpp"
#include "name_generator.hpp"
#include "tar.hpp"

#include <big/write.hpp>

#include <disposer/module.hpp>
#include <disposer/type_position.hpp>

#include <boost/dll.hpp>


namespace disposer_module{ namespace big_saver{


	enum class input_t{
		sequence,
		vector,
		image
	};

	struct parameter{
		bool tar;

		std::size_t sequence_start;
		std::size_t camera_start;

		std::string dir;

		std::optional< std::size_t > fixed_id;

		std::size_t sequence_count;

		input_t input;

		std::optional< name_generator< std::size_t > > tar_pattern;
		std::optional< name_generator<
			std::size_t, std::size_t, std::size_t > > big_pattern;
	};

	using save_image = std::variant<
			std::reference_wrapper< bitmap< std::int8_t > const >,
			std::reference_wrapper< bitmap< std::uint8_t > const >,
			std::reference_wrapper< bitmap< std::int16_t > const >,
			std::reference_wrapper< bitmap< std::uint16_t > const >,
			std::reference_wrapper< bitmap< std::int32_t > const >,
			std::reference_wrapper< bitmap< std::uint32_t > const >,
			std::reference_wrapper< bitmap< std::int64_t > const >,
			std::reference_wrapper< bitmap< std::uint64_t > const >,
			std::reference_wrapper< bitmap< float > const >,
			std::reference_wrapper< bitmap< double > const >,
			std::reference_wrapper< bitmap< long double > const >
		>;

	using save_vector = std::vector< save_image >;

	using save_sequence = std::vector< save_vector >;


	struct module: disposer::module_base{
		module(disposer::make_data const& data, parameter&& param):
			disposer::module_base(data, {sequence, vector, image}),
			param(std::move(param))
			{}


		using types = disposer::type_list<
			std::int8_t,
			std::uint8_t,
			std::int16_t,
			std::uint16_t,
			std::int32_t,
			std::uint32_t,
			std::int64_t,
			std::uint64_t,
			float,
			double,
			long double
		>;


		disposer::container_input< bitmap_sequence, types >
			sequence{"sequence"};

		disposer::container_input< bitmap_vector, types >
			vector{"vector"};

		disposer::container_input< bitmap, types >
			image{"image"};


		void save(std::size_t id, save_sequence const& bitmap_sequence);


		std::string tar_name(std::size_t id);
		std::string big_name(std::size_t cam, std::size_t pos);
		std::string big_name(std::size_t id, std::size_t cam, std::size_t pos);


		template < typename T >
		void exec_sequence(std::size_t id);

		template < typename T >
		void exec_vector(std::size_t id);

		template < typename T >
		void exec_image(std::size_t id);


		void exec()override;


		parameter const param;
	};


	disposer::module_ptr make_module(disposer::make_data& data){
		if(data.is_first()) throw disposer::module_not_as_start(data);

		std::array< bool, 3 > const use_input{{
			data.inputs.find("sequence") != data.inputs.end(),
			data.inputs.find("vector") != data.inputs.end(),
			data.inputs.find("image") != data.inputs.end()
		}};

		auto input_count = std::count(use_input.begin(), use_input.end(), true);

		if(input_count > 1){
			throw std::logic_error(
				"can only use one input ('image', 'vector' or 'sequence')");
		}

		if(input_count == 0){
			throw std::logic_error(
				"no input defined (use 'image', 'vector' or 'sequence')");
		}

		auto& params = data.params;
		auto param = parameter();

		params.set(param.tar, "tar", false);

		params.set(param.sequence_start, "sequence_start", 0);
		params.set(param.camera_start, "camera_start", 0);

		params.set(param.dir, "dir", ".");

		auto id_digits = params.get("id_digits", std::size_t(3));
		auto camera_digits = params.get("camera_digits", std::size_t(1));
		auto position_digits = params.get("position_digits", std::size_t(3));

		params.set(param.fixed_id, "fixed_id");

		if(use_input[0]){
			param.input = input_t::sequence;
		}else if(use_input[1]){
			param.input = input_t::vector;
		}else{
			param.input = input_t::image;
			params.set(param.sequence_count, "sequence_count");
			if(param.sequence_count == 0){
				throw std::logic_error(make_string(
					"sequence_count (value: ", param.sequence_count,
					") needs to be greater than 0"));
			}
		}

		struct format{
			std::size_t digits;

			std::string operator()(std::size_t value){
				std::ostringstream os;
				os << std::setw(digits) << std::setfill('0') << value;
				return os.str();
			}
		};

		if(param.tar){
			param.tar_pattern = make_name_generator(
				params.get< std::string >("tar_pattern", "${id}.tar"),
				{{true}},
				std::make_pair("id", format{id_digits})
			);
		}

		param.big_pattern = make_name_generator(
			params.get("big_pattern", param.tar
				? std::string("${cam}_${pos}.big")
				: std::string("${id}_${cam}_${pos}.big")),
			{{!param.tar, true, true}},
			std::make_pair("id", format{id_digits}),
			std::make_pair("cam", format{camera_digits}),
			std::make_pair("pos", format{position_digits})
		);

		return std::make_unique< module >(data, std::move(param));
	}


	struct big_stream_visitor{
		big_stream_visitor(std::ostream& os): os(os){}

		std::ostream& os;

		template < typename T >
		void operator()(T const& image_ref)const{
			big::write(image_ref.get(), os);
		}
	};

	struct big_streamsize_visitor{
		template < typename T >
		std::size_t operator()(T const& image_ref)const{
			using value_type = std::remove_cv_t< std::remove_pointer_t<
				decltype(image_ref.get().data()) > >;

			auto content_bytes = image_ref.get().width() *
				image_ref.get().height() * sizeof(value_type);

			return 10 + content_bytes;
		}
	};

	struct big_file_visitor{
		big_file_visitor(std::string const& name): name(name){}

		std::string const& name;

		template < typename T >
		void operator()(T const& image_ref)const{
			big::write(image_ref.get(), name);
		}
	};


	void module::save(std::size_t id, save_sequence const& bitmap_sequence){
		auto used_id = param.fixed_id ? *param.fixed_id : id;

		if(param.tar){
			auto tarname = param.dir + "/" + (*param.tar_pattern)(used_id);
			log([this, &tarname, id](disposer::log_base& os){
				os << "write '" << tarname << "'";
			},
			[this, id, used_id, &bitmap_sequence, &tarname]{
				tar_writer tar(tarname);
				std::size_t cam = param.camera_start;
				for(auto& sequence: bitmap_sequence){
					std::size_t pos = param.sequence_start;
					for(auto& bitmap: sequence){
						auto filename = (*param.big_pattern)(used_id, cam, pos);
						log([this, &tarname, &filename, id](
							disposer::log_base& os
						){
							os << "write '" << tarname << "/" << filename
								<< "'";
						},
						[&tar, &bitmap, &filename]{
							tar.write(filename, [&bitmap](std::ostream& os){
								std::visit(big_stream_visitor(os), bitmap);
							}, std::visit(big_streamsize_visitor(), bitmap));
						});
						++pos;
					}
					++cam;
				}
			});
		}else{
			std::size_t cam = param.camera_start;
			for(auto& sequence: bitmap_sequence){
				std::size_t pos = param.sequence_start;
				for(auto& bitmap: sequence){
					auto filename = param.dir + "/" +
						(*param.big_pattern)(used_id, cam, pos);
					log([this, &filename, id](disposer::log_base& os){
						os << "write '" << filename << "'";
					},
					[&bitmap, &filename]{
						std::visit(big_file_visitor(filename), bitmap);
					});
					++pos;
				}
				++cam;
			}
		}
	}


	struct sequence_visitor{
		sequence_visitor(std::size_t id): id(id){}

		std::size_t const id;

		template < typename T >
		save_sequence operator()(T const& sequence)const{
			save_sequence data;
			for(auto& vector: sequence.data()){
				data.emplace_back();
				for(auto& bitmap: vector){
					data.back().emplace_back(bitmap);
				}
			}

			return data;
		}
	};

	struct vector_visitor{
		template < typename T >
		save_vector operator()(T const& vectors)const{
			save_vector result;
			for(auto& image: vectors.data()){
				result.emplace_back(image);
			}

			return result;
		}
	};

	struct image_visitor{
		template < typename T >
		save_image operator()(T const& image)const{
			return image.data();
		}
	};


	void module::exec(){
		switch(param.input){
			case input_t::sequence:{
				for(auto const& pair: sequence.get()){
					auto id = pair.first;
					auto& bitmap_sequence = pair.second;

					save(id, std::visit(sequence_visitor(id), bitmap_sequence));
				}
			}break;
			case input_t::vector:{
				auto vectors = vector.get();
				auto from = vectors.begin();

				while(from != vectors.end()){
					auto id = from->first;
					auto to = vectors.upper_bound(id);

					save_sequence data;
					for(auto iter = from; iter != to; ++iter){
						data.emplace_back(std::visit(vector_visitor(),
							iter->second));
					}

					save(id, data);

					from = to;
				}
			}break;
			case input_t::image:{
				auto images = image.get();
				auto from = images.begin();

				while(from != images.end()){
					auto id = from->first;
					auto to = images.upper_bound(id);

					std::size_t pos = 0;
					save_sequence data;
					for(auto iter = from; iter != to; ++iter){
						if(pos == 0) data.emplace_back();
						++pos;

						if(pos == param.sequence_count) pos = 0;

						data.back().emplace_back(std::visit(
							image_visitor(), iter->second));
					}

					if(pos != 0){
						throw std::runtime_error(
							"single image count does not match parameter "
							"'sequence_count'");
					}

					save(id, data);

					from = to;
				}
			}break;
		}
	}


	void init(disposer::module_declarant& add){
		add("big_saver", &make_module);
	}

	BOOST_DLL_AUTO_ALIAS(init)


} }
