require 'mongo'

include Mongo

$stdout.sync = true

client = MongoClient.new(ENV['MONGODB'], 27017)
db = client["test"]
collection = db["customers"]

loop do
print ">:t:aggregate::\n"
    collection.aggregate( [
                        { "$match" => {}},
                        { "$group" => {
                                      "_id" => "$country_code",
                                      "orders_count" => { "$sum" => "$orders_count" }
                                    }
                       }
               ])
print "<:t:aggregate::\n"
end
